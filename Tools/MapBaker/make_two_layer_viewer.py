#!/usr/bin/env python3
"""Generate the final two-layer viewer: opaque scene + transparent minimap quads."""
import argparse
import os

HTML = r'''<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>@TITLE@</title>
<style>
  :root { color-scheme: dark; }
  html, body { margin:0; height:100%; overflow:hidden; background:#0b1620;
               font:13px/1.45 system-ui,sans-serif; color:#d7e6f5; }
  #checker { position:fixed; inset:0; z-index:0;
    background-color:#0b1620;
    background-image:
      linear-gradient(45deg,#142433 25%,transparent 25%,transparent 75%,#142433 75%),
      linear-gradient(45deg,#142433 25%,transparent 25%,transparent 75%,#142433 75%);
    background-size:32px 32px; background-position:0 0,16px 16px; }
  #osd { position:fixed; inset:0; z-index:1; background:transparent; }
  #panel { position:fixed; left:14px; top:14px; z-index:10; width:270px;
    background:rgba(9,20,30,.94); border:1px solid #23384d; border-radius:9px;
    padding:12px 13px; box-shadow:0 8px 30px rgba(0,0,0,.35); }
  #panel h1 { margin:0 0 2px; font-size:15px; color:#9fd0ff; font-weight:650; }
  #panel .sub { color:#7f96ab; font-size:11px; margin-bottom:10px; }
  .layer { display:flex; align-items:center; gap:5px; margin:6px 0; }
  .toggle { flex:1; text-align:left; padding:8px 10px; border-radius:6px;
    border:1px solid #2a4157; background:#0f1d2a; color:#8fa6ba; cursor:pointer;
    font:inherit; transition:background .12s,color .12s,border-color .12s; }
  .toggle:hover { border-color:#3d6285; color:#c7ddf0; }
  .toggle.on { background:#123a5c; border-color:#3d8bd4; color:#eaf6ff;
    box-shadow:inset 0 0 0 1px rgba(61,139,212,.35); }
  .toggle .dot { display:inline-block; width:7px; height:7px; border-radius:50%;
    margin-right:7px; background:#3b4d5e; vertical-align:1px; }
  .toggle.on .dot { background:#5fd18b; box-shadow:0 0 6px #5fd18b; }
  .slider { display:flex; align-items:center; gap:8px; margin:3px 0 8px 8px; }
  .slider input { flex:1; accent-color:#3d8bd4; }
  .slider output { width:34px; text-align:right; color:#9fd0ff; font:11px monospace; }
  .hint { color:#667d91; font-size:11px; margin-top:9px; border-top:1px solid #1e3144;
          padding-top:8px; }
  .reset { margin-top:7px; padding:5px 8px; border-radius:5px; border:1px solid #2a4157;
    background:#0f1d2a; color:#8fa6ba; cursor:pointer; font:11px inherit; }
  .reset:hover { border-color:#3d6285; color:#c7ddf0; }
  #hud { position:fixed; left:14px; bottom:14px; z-index:10; color:#b7cde0;
    font:11px/1.5 Consolas,monospace; background:rgba(0,0,0,.55);
    padding:7px 10px; border-radius:5px; pointer-events:none; white-space:pre; }
  #hud b { color:#9fd0ff; font-weight:600; }
  #err { color:#ff8b8b; font-size:11px; margin-top:6px; white-space:pre-wrap; }
</style>
</head>
<body>
<div id="checker"></div>
<div id="osd"></div>
<div id="panel">
  <h1>@TITLE@</h1>
  <div class="sub">@PX@ px/ADT · @COUNT@ tiles · map @MAPID@</div>

  <div class="layer">
    <button class="toggle on" id="t-scene" data-layer="scene" aria-pressed="true"><span class="dot"></span>Scene (terrain + WMO + trees)</button>
  </div>
  <div class="layer">
    <button class="toggle on" id="t-quads" data-layer="quads" aria-pressed="true"><span class="dot"></span>Minimap quads</button>
  </div>
  <div class="slider">
    <input id="qopacity" type="range" min="0" max="100" value="100">
    <output id="qopacityv">100%</output>
  </div>
  <button class="reset" id="reset">Reset layers</button>
  <div class="hint">Scene off shows the checkerboard.<br>Quads are drawn above the scene.</div>
  <div id="err"></div>
</div>
<div id="hud">loading…</div>

<script>
const TX0=@TX0@, TY0=@TY0@, PX=@PX@, MAPID=@MAPID@;
const A=533.33331, O=17066.666;
const LAYERS=[
  {id:'scene', file:'@NAME@-scene.dzi'},
  {id:'quads', file:'@NAME@-quads.dzi'}
];
const state={visible:{scene:true,quads:true}, quadsOpacity:1.0, items:{}};
let viewer=null, loaded=0;

function updateButtons(){
  for (const id of ['scene','quads']) {
    const b=document.getElementById('t-'+id);
    b.classList.toggle('on', !!state.visible[id]);
    b.setAttribute('aria-pressed', state.visible[id] ? 'true' : 'false');
  }
  const q=document.getElementById('qopacity');
  q.value=Math.round(state.quadsOpacity*100);
  document.getElementById('qopacityv').textContent=Math.round(state.quadsOpacity*100)+'%';
}
function applyLayer(id){
  const item=state.items[id];
  if (!item) return;
  let op=0;
  if (state.visible[id]) op=(id==='quads') ? state.quadsOpacity : 1;
  item.setOpacity(op);
}
function applyAll(){
  applyLayer('scene'); applyLayer('quads');
  if (viewer) { viewer.world.setItemIndex(state.items.scene,0); viewer.world.setItemIndex(state.items.quads,1); viewer.forceRedraw(); }
  updateButtons();
}
function setVisible(id,on){
  state.visible[id]=!!on;
  if (id==='quads' && on && state.quadsOpacity===0) state.quadsOpacity=1;
  applyLayer(id); updateButtons();
}
function initOSD(){
  viewer=OpenSeadragon({
    id:'osd',
    prefixUrl:'openseadragon/images/',
    showNavigator:true,navigatorPosition:'BOTTOM_RIGHT',navigatorHeight:190,navigatorWidth:130,
    gestureSettingsMouse:{clickToZoom:false,dblClickToZoom:true},
    maxZoomPixelRatio:3,visibilityRatio:1,constrainDuringPan:true,
    animationTime:0.35,blendTime:0.1,imageLoaderLimit:12,timeout:120000,
    backgroundColor:'rgba(0,0,0,0)',showRotationControl:false
  });
  LAYERS.forEach((l,index)=>{
    viewer.addTiledImage({
      tileSource:l.file, index:index,
      success:ev=>{
        state.items[l.id]=ev.item;
        loaded++;
        applyLayer(l.id);
        if (loaded===LAYERS.length) applyAll();
      },
      error:ev=>{
        document.getElementById('err').textContent='Could not load '+l.file;
        console.error('layer load failed',l.file,ev);
      }
    });
  });
  const hud=document.getElementById('hud');
  function update(){
    if (!viewer) return;
    const img=viewer.world.getItemAt(0); if (!img) return;
    const c=viewer.viewport.getCenter(true);
    const p=img.viewportToImageCoordinates(c);
    const ix=p.x, iy=p.y;
    const tx=TX0+Math.floor(ix/PX), ty=TY0+Math.floor(iy/PX);
    const wy=O-(TX0+ix/PX)*A, wx=O-(TY0+iy/PX)*A;
    hud.innerHTML='<b>map '+MAPID+'</b>  ADT <b>('+tx+', '+ty+')</b>\n'+
      'world <b>'+wx.toFixed(1)+', '+wy.toFixed(1)+'</b>\n'+
      'image '+ix.toFixed(0)+', '+iy.toFixed(0)+' px   zoom '+
      (viewer.viewport.getZoom(true)*100).toFixed(0)+'%';
  }
  viewer.addHandler('open',update);
  ['animation','animation-finish','resize','zoom','pan','canvas-scroll'].forEach(e=>viewer.addHandler(e,update));
  window.addEventListener('keydown',e=>{ if(e.key==='f'||e.key==='F') viewer.viewport.goHome(); });

  document.querySelectorAll('.toggle').forEach(b=>{
    b.addEventListener('click',()=>setVisible(b.dataset.layer,!state.visible[b.dataset.layer]));
  });
  document.getElementById('qopacity').addEventListener('input',e=>{
    state.quadsOpacity=Number(e.target.value)/100;
    state.visible.quads = state.quadsOpacity>0;
    applyLayer('quads'); updateButtons();
  });
  document.getElementById('reset').addEventListener('click',()=>{
    state.visible={scene:true,quads:true}; state.quadsOpacity=1; applyAll();
  });
  updateButtons();
}
(function(){
  const s=document.createElement('script');
  s.src='openseadragon/openseadragon.min.js';
  s.onload=initOSD;
  s.onerror=function(){ document.getElementById('err').textContent='OpenSeadragon failed to load'; };
  document.head.appendChild(s);
})();
</script>
</body>
</html>
'''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', required=True)
    ap.add_argument('--name', required=True)
    ap.add_argument('--map-id', type=int, required=True)
    ap.add_argument('--tx0', type=int, required=True)
    ap.add_argument('--ty0', type=int, required=True)
    ap.add_argument('--count', type=int, default=0)
    ap.add_argument('--px', type=int, default=1024)
    ap.add_argument('--title', default=None)
    args = ap.parse_args()

    title = args.title or ('%s — map %d' % (args.name, args.map_id))
    html = (HTML.replace('@NAME@', args.name)
                .replace('@TITLE@', title)
                .replace('@MAPID@', str(args.map_id))
                .replace('@TX0@', str(args.tx0))
                .replace('@TY0@', str(args.ty0))
                .replace('@COUNT@', str(args.count))
                .replace('@PX@', str(args.px)))
    path = os.path.join(args.out, 'viewer.html')
    with open(path, 'w', encoding='utf-8') as f:
        f.write(html)
    print('wrote %s' % path)


if __name__ == '__main__':
    main()
