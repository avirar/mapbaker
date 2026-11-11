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
  #cross { position:fixed; left:50%; top:50%; z-index:9; pointer-events:none;
    display:block; width:0; height:0; }
  #cross:before, #cross:after { content:""; position:absolute; background:#fff;
    box-shadow:0 0 0 1px rgba(0,0,0,.85), 0 0 4px rgba(0,0,0,.6); }
  #cross:before { left:-15px; top:-1px; width:31px; height:2px; }
  #cross:after { left:-1px; top:-15px; width:2px; height:31px; }
  #copy { width:100%; margin-top:7px; padding:6px 8px; border-radius:5px;
    border:1px solid #2a4157; background:#0f1d2a; color:#9fd0ff; cursor:pointer;
    font:12px inherit; }
  #copy:hover { border-color:#3d6285; background:#123a5c; }
</style>
</head>
<body>
<div id="checker"></div>
<div id="osd"></div>
<div id="cross"></div>
<div id="panel">
  <h1>@TITLE@</h1>
  <div class="sub">@PX@ px/ADT · @COUNT@ tiles · map @MAPID@</div>

  <div class="layer">
    <button class="toggle on" id="t-scene" data-layer="scene" aria-pressed="true"><span class="dot"></span>Scene (terrain + WMO + trees)</button>
  </div>
  <div class="layer">
    <button class="toggle on" id="t-quads-ext" data-layer="quads-ext" aria-pressed="false"><span class="dot"></span>Minimap quads — exterior</button>
  </div>
  <div class="layer">
    <button class="toggle on" id="t-quads-int" data-layer="quads-int" aria-pressed="true"><span class="dot"></span>Minimap quads — interior</button>
  </div>
  <div class="layer">
    <button class="toggle on" id="t-interiors" data-layer="interiors" aria-pressed="true"><span class="dot"></span>Interior remaster</button>
  </div>
  <div class="layer">
    <button class="toggle on" id="t-cross" aria-pressed="true"><span class="dot"></span>Crosshair</button>
  </div>
  <div class="slider">
    <input id="qopacity" type="range" min="0" max="100" value="100">
    <output id="qopacityv">100%</output>
  </div>
  <button class="reset" id="reset">Reset layers</button>
  <button id="copy">Copy coords</button>
  <div class="hint">Interior remaster + interior quads.<br>Exterior quads are optional; toggle either.<br>Crosshair is fixed at the centre.</div>
  <div id="err"></div>
</div>
<div id="hud">loading…</div>

<script>
const TX0=@TX0@, TY0=@TY0@, PX=@PX@, MAPID=@MAPID@, FIT=@FIT@;
const A=533.33331, O=17066.666;
const LAYERS=[
  {id:'scene', file:'@NAME@-scene.dzi'},
  {id:'interiors', file:'@NAME@-interiors.dzi'},
  {id:'quads-ext', file:'@NAME@-quads-ext.dzi'},
  {id:'quads-int', file:'@NAME@-quads-int.dzi'}
];
const state={visible:{scene:true,interiors:true,'quads-ext':false,'quads-int':true}, crosshair:true, quadsOpacity:1.0, items:{}};
let viewer=null, loaded=0;

function updateButtons(){
  for (const id of ['scene','interiors','quads-ext','quads-int']) {
    const b=document.getElementById('t-'+id);
    b.classList.toggle('on', !!state.visible[id]);
    b.setAttribute('aria-pressed', state.visible[id] ? 'true' : 'false');
  }
  const t=document.getElementById('t-cross');
  t.classList.toggle('on', !!state.crosshair);
  t.setAttribute('aria-pressed', state.crosshair ? 'true' : 'false');
  document.getElementById('cross').style.display = state.crosshair ? 'block' : 'none';
  const q=document.getElementById('qopacity');
  q.value=Math.round(state.quadsOpacity*100);
  document.getElementById('qopacityv').textContent=Math.round(state.quadsOpacity*100)+'%';
}
function applyLayer(id){
  const item=state.items[id];
  if (!item) return;
  let op=0;
  if (state.visible[id]) op=(id==='quads-ext' || id==='quads-int') ? state.quadsOpacity : 1;
  item.setOpacity(op);
}
function applyAll(){
  applyLayer('scene'); applyLayer('interiors'); applyLayer('quads-ext'); applyLayer('quads-int');
  if (viewer) {
    viewer.world.setItemIndex(state.items.scene,0);
    viewer.world.setItemIndex(state.items.interiors,1);
    viewer.world.setItemIndex(state.items['quads-ext'],2);
    viewer.world.setItemIndex(state.items['quads-int'],3);
    viewer.forceRedraw();
  }
  updateButtons();
}
function setVisible(id,on){
  state.visible[id]=!!on;
  if ((id==='quads-ext' || id==='quads-int') && on && state.quadsOpacity===0) state.quadsOpacity=1;
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
        if (loaded===LAYERS.length) {
          applyAll();
          if (FIT) {
            setTimeout(()=>{
              const item=viewer.world.getItemAt(0);
              if (!item) return;
              const r=item.imageToViewportRectangle(
                new OpenSeadragon.Rect(FIT[0],FIT[1],FIT[2],FIT[3]));
              viewer.viewport.fitBounds(r, true);
            }, 50);
          }
        }
      },
      error:ev=>{
        document.getElementById('err').textContent='Could not load '+l.file;
        console.error('layer load failed',l.file,ev);
      }
    });
  });
  const hud=document.getElementById('hud');
  const cross=document.getElementById('cross');
  let lastCoords=null;
  function coordsAt(ix,iy){
    const tx=TX0+Math.floor(ix/PX), ty=TY0+Math.floor(iy/PX);
    const wy=O-(TX0+ix/PX)*A, wx=O-(TY0+iy/PX)*A;
    return {ix:ix,iy:iy,tx:tx,ty:ty,wx:wx,wy:wy};
  }
  function hudText(c){
    return '<b>map '+MAPID+'</b>  ADT <b>('+c.tx+', '+c.ty+')</b>\n'+
      'world <b>'+c.wx.toFixed(1)+', '+c.wy.toFixed(1)+'</b>\n'+
      'image '+c.ix.toFixed(0)+', '+c.iy.toFixed(0)+' px   zoom '+
      (viewer.viewport.getZoom(true)*100).toFixed(0)+'%';
  }
  function centerCoords(){
    const img=viewer.world.getItemAt(0);
    if (!img) return null;
    const c=viewer.viewport.getCenter(true);
    const p=img.viewportToImageCoordinates(c);
    return coordsAt(p.x,p.y);
  }
  function updateCenter(){
    if (!viewer) return;
    const c=centerCoords();
    if (!c) return;
    lastCoords=c;
    hud.innerHTML=hudText(c);
  }
  viewer.addHandler('open',updateCenter);
  ['animation','animation-finish','resize','zoom','pan','canvas-scroll'].forEach(e=>viewer.addHandler(e,updateCenter));
  window.addEventListener('keydown',e=>{ if(e.key==='f'||e.key==='F') viewer.viewport.goHome(); });

  function copyText(text){
    if (navigator.clipboard && window.isSecureContext) {
      navigator.clipboard.writeText(text).catch(()=>fallbackCopy(text));
    } else {
      fallbackCopy(text);
    }
  }
  function fallbackCopy(text){
    const ta=document.createElement('textarea');
    ta.value=text; ta.style.position='fixed'; ta.style.left='-9999px';
    document.body.appendChild(ta); ta.select();
    try { document.execCommand('copy'); } catch(e) {}
    ta.remove();
  }
  document.querySelectorAll('.toggle[data-layer]').forEach(b=>{
    b.addEventListener('click',()=>setVisible(b.dataset.layer,!state.visible[b.dataset.layer]));
  });
  document.getElementById('qopacity').addEventListener('input',e=>{
    state.quadsOpacity=Number(e.target.value)/100;
    applyLayer('quads-ext'); applyLayer('quads-int'); updateButtons();
  });
  document.getElementById('t-cross').addEventListener('click',()=>{
    state.crosshair=!state.crosshair; updateButtons();
  });
  document.getElementById('copy').addEventListener('click',()=>{
    // Capture the centre at click time, not the cursor position.
    const c=centerCoords() || lastCoords;
    const txt=c ? ('Map '+MAPID+' | ADT ('+c.tx+', '+c.ty+') | world '+
      c.wx.toFixed(1)+', '+c.wy.toFixed(1)+' | image '+
      c.ix.toFixed(0)+', '+c.iy.toFixed(0)+' px') : ('Map '+MAPID);
    copyText(txt);
    const b=document.getElementById('copy');
    b.textContent='Copied'; setTimeout(()=>{ b.textContent='Copy coords'; },1200);
  });
  document.getElementById('reset').addEventListener('click',()=>{
    state.visible={scene:true,interiors:true,'quads-ext':false,'quads-int':true}; state.quadsOpacity=1; applyAll();
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
    ap.add_argument('--fit', default='', help='initial image-pixel fit: x,y,w,h')
    ap.add_argument('--px', type=int, default=1024)
    ap.add_argument('--title', default=None)
    args = ap.parse_args()

    fit = 'null'
    if args.fit:
        fit = '[' + ','.join(x.strip() for x in args.fit.split(',')) + ']'
    title = args.title or ('%s — map %d' % (args.name, args.map_id))
    html = (HTML.replace('@NAME@', args.name)
                .replace('@TITLE@', title)
                .replace('@MAPID@', str(args.map_id))
                .replace('@TX0@', str(args.tx0))
                .replace('@TY0@', str(args.ty0))
                .replace('@COUNT@', str(args.count))
                .replace('@FIT@', fit)
                .replace('@PX@', str(args.px)))
    path = os.path.join(args.out, 'viewer-interiors.html')
    with open(path, 'w', encoding='utf-8') as f:
        f.write(html)
    print('wrote %s' % path)


if __name__ == '__main__':
    main()
