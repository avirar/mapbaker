#!/usr/bin/env python3
"""Unified MapBaker viewer: scene, interior remaster, stock minimap quads, and
per-group interior-art x-ray for instances/battlegrounds.

Layers:
    <name>-scene.dzi       opaque 3D scene
    <name>-interiors.dzi   transparent 3D interiors (stock interior locations only)
    <name>-quads-ext.dzi   stock outdoor minimap tiles (map<tx>_<ty>.blp)
    <name>-quads-int.dzi   stock interior WMO art cells (whole-map x-ray)

Instances/battlegrounds additionally load interior-art.json + interior-art/*.png,
which lets the user tick individual WMO groups and draw their stock minimap art
on top of the scene with the correct WMO instance transform.
"""
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
  #artcanvas { position:fixed; inset:0; z-index:2; pointer-events:none; }
  #panel { position:fixed; left:14px; top:14px; z-index:10; width:278px;
    background:rgba(9,20,30,.94); border:1px solid #23384d; border-radius:9px;
    padding:12px 13px; box-shadow:0 8px 30px rgba(0,0,0,.35); }
  #panel h1 { margin:0 0 2px; font-size:15px; color:#9fd0ff; font-weight:650; }
  #panel .sub { color:#7f96ab; font-size:11px; margin-bottom:10px; }
  .layer { display:flex; align-items:center; gap:5px; margin:6px 0; }
  .toggle { flex:1; text-align:left; padding:8px 10px; border-radius:6px;
    border:1px solid #2a4157; background:#0f1d2a; color:#8fa6ba; cursor:pointer;
    font:inherit; transition:background .12s,color .12s,border-color .12s; }
  .toggle:hover:not(:disabled) { border-color:#3d6285; color:#c7ddf0; }
  .toggle.on { background:#123a5c; border-color:#3d8bd4; color:#eaf6ff;
    box-shadow:inset 0 0 0 1px rgba(61,139,212,.35); }
  .toggle:disabled { opacity:.45; cursor:default; }
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

  /* per-group interior-art x-ray panel (instances / battlegrounds) */
  #gpanel { position:fixed; right:14px; top:14px; bottom:14px; width:336px; z-index:11;
    background:rgba(9,20,30,.96); border:1px solid #23384d; border-radius:9px;
    box-shadow:0 8px 30px rgba(0,0,0,.4); display:none; flex-direction:column;
    overflow:hidden; }
  #gpanel.show { display:flex; }
  .ghead { display:flex; align-items:center; gap:8px; padding:10px 12px;
    border-bottom:1px solid #1e3144; }
  .ghead b { color:#9fd0ff; font-size:14px; flex:1; }
  .gclose { width:26px; height:26px; border-radius:5px; border:1px solid #2a4157;
    background:#0f1d2a; color:#8fa6ba; cursor:pointer; font:16px/1 monospace; }
  .gclose:hover { border-color:#3d6285; color:#c7ddf0; }
  .gsub { color:#7f96ab; font-size:11px; padding:8px 12px 0; }
  .grow { display:flex; gap:7px; padding:8px 12px 0; }
  .grow input[type=text] { flex:1; background:#0f1d2a; color:#d7e6f5;
    border:1px solid #2a4157; border-radius:5px; padding:6px 8px; font:12px inherit; }
  .grow button { padding:5px 8px; border-radius:5px; border:1px solid #2a4157;
    background:#0f1d2a; color:#8fa6ba; cursor:pointer; font:11px inherit; white-space:nowrap; }
  .grow button:hover { border-color:#3d6285; color:#c7ddf0; }
  #ghit { padding:8px 12px 0; display:flex; flex-wrap:wrap; gap:5px; }
  #ghit .none { color:#667d91; font-size:11px; }
  .chip { padding:4px 7px; border-radius:12px; border:1px solid #2a4157;
    background:#0f1d2a; color:#9fd0ff; cursor:pointer; font:11px inherit; }
  .chip:hover { border-color:#3d8bd4; background:#123a5c; }
  .chip.on { background:#123a5c; border-color:#3d8bd4; color:#eaf6ff; }
  #glist { flex:1; overflow:auto; padding:6px 8px 10px; margin-top:4px; }
  .gitem { display:flex; align-items:flex-start; gap:7px; padding:6px 5px;
    border-radius:5px; cursor:pointer; }
  .gitem:hover { background:#101f2d; }
  .gitem input { margin-top:2px; accent-color:#3d8bd4; }
  .gitem b { color:#cfe5f7; font-weight:600; font-size:12px; }
  .gitem small { display:block; color:#6f879b; font-size:10px; }
  .gitem.hit { box-shadow:inset 0 0 0 1px #3d8bd4; }
  #gerr { color:#ff8b8b; font-size:11px; padding:8px 12px; white-space:pre-wrap; }
  @media (max-width:900px) {
    #gpanel { width:300px; }
    #panel { width:250px; }
  }
</style>
</head>
<body>
<div id="checker"></div>
<div id="osd"></div>
<canvas id="artcanvas"></canvas>
<div id="cross"></div>
<div id="panel">
  <h1>@TITLE@</h1>
  <div class="sub">@PX@ px/ADT · @COUNT@ tiles · map @MAPID@</div>

  <div class="layer">
    <button class="toggle on" id="t-scene" data-layer="scene" aria-pressed="true"><span class="dot"></span>Scene (terrain + WMO + trees)</button>
  </div>
  <div class="layer">
    <button class="toggle on" id="t-interiors" data-layer="interiors" aria-pressed="true"><span class="dot"></span>Interior remaster</button>
  </div>
  <div class="layer">
    <button class="toggle" id="t-quads-ext" data-layer="quads-ext" aria-pressed="false"><span class="dot"></span>Minimap quads — exterior</button>
  </div>
  <div class="slider">
    <input id="qextopacity" type="range" min="0" max="100" value="100">
    <output id="qextopacityv">100%</output>
  </div>
  <div class="layer">
    <button class="toggle" id="t-quads-int" data-layer="quads-int" aria-pressed="false"><span class="dot"></span>Minimap quads — interior</button>
  </div>
  <div class="slider">
    <input id="qintopacity" type="range" min="0" max="100" value="100">
    <output id="qintopacityv">100%</output>
  </div>
  <div class="layer" id="groupsrow" style="display:none">
    <button class="toggle" id="t-groups" aria-pressed="false"><span class="dot"></span>Interior tunnel groups…</button>
  </div>
  <div class="layer">
    <button class="toggle on" id="t-cross" aria-pressed="true"><span class="dot"></span>Crosshair</button>
  </div>
  <button class="reset" id="reset">Reset layers</button>
  <button id="copy">Copy coords</button>
  <div class="hint">Scene + interior remaster by default.<br>
    Exterior quads show the original stitched minimap.<br>
    Interior quads are the whole-map tunnel x-ray.<br>
    Instance tunnel groups can be ticked individually.</div>
  <div id="err"></div>
</div>

<div id="gpanel">
  <div class="ghead"><b>Interior tunnel x-ray</b><button class="gclose" id="gclose">×</button></div>
  <div class="gsub">Tick stock interior WMO groups to draw their minimap art over the scene. The group is pinned/rotated with the WMO instance matrix.</div>
  <div class="grow"><input type="text" id="gsearch" placeholder="Search group, WMO or number"></div>
  <div class="grow"><button id="gclear">Clear</button><button id="gat">Select at crosshair</button></div>
  <div id="ghit"></div>
  <div id="glist"></div>
  <div id="gerr"></div>
</div>

<div id="hud">loading…</div>

<script>
const TX0=@TX0@, TY0=@TY0@, PX=@PX@, MAPID=@MAPID@, FIT=@FIT@, INSTANCE=@INSTANCE@;
const HAS_EXT=@HAS_EXT@, HAS_GROUPS=@HAS_GROUPS@;
const A=533.33331, O=17066.666;
const LAYERS=[
  {id:'scene', file:'@NAME@-scene.dzi'},
  {id:'interiors', file:'@NAME@-interiors.dzi'},
  ...(HAS_EXT ? [{id:'quads-ext', file:'@NAME@-quads-ext.dzi'}] : []),
  {id:'quads-int', file:'@NAME@-quads-int.dzi'}
];
const DEFAULTS={scene:true,interiors:true,'quads-ext':false,'quads-int':false};
const state={
  visible:Object.assign({},DEFAULTS),
  extOpacity:1.0, intOpacity:1.0,
  crosshair:true, items:{}, groups:[], selected:new Set(),
  groupFilter:'', panelOpen:false, fitted:false
};
let viewer=null, loaded=0, pendingDraw=false, hitTimer=null;

// Optional share/debug override: ?layers=quads-ext,quads-int
(function(){
  const q=new URLSearchParams(location.search).get('layers');
  if (!q) return;
  for (const id of q.split(',')) {
    const t=id.trim();
    if (t in state.visible) state.visible[t]=true;
  }
})();

function $(id){ return document.getElementById(id); }
function updateButtons(){
  for (const id of ['scene','interiors','quads-ext','quads-int']) {
    const b=$('t-'+id); if(!b) continue;
    b.classList.toggle('on', !!state.visible[id]);
    b.setAttribute('aria-pressed', state.visible[id] ? 'true' : 'false');
  }
  const t=$('t-cross');
  t.classList.toggle('on', !!state.crosshair);
  t.setAttribute('aria-pressed', state.crosshair ? 'true' : 'false');
  $('cross').style.display = state.crosshair ? 'block' : 'none';
  $('t-groups').classList.toggle('on', state.panelOpen);
  $('t-groups').setAttribute('aria-pressed', state.panelOpen ? 'true' : 'false');
  $('qextopacity').value=Math.round(state.extOpacity*100);
  $('qextopacityv').textContent=Math.round(state.extOpacity*100)+'%';
  $('qintopacity').value=Math.round(state.intOpacity*100);
  $('qintopacityv').textContent=Math.round(state.intOpacity*100)+'%';
}
function applyLayer(id){
  const item=state.items[id]; if(!item) return;
  let op=0;
  if (state.visible[id]) {
    if (id==='quads-ext') op=state.extOpacity;
    else if (id==='quads-int') op=state.intOpacity;
    else op=1;
  }
  item.setOpacity(op);
}
function applyAll(){
  for (const id of ['scene','interiors','quads-ext','quads-int']) applyLayer(id);
  if (viewer) {
    const order=['scene','interiors','quads-ext','quads-int'];
    order.forEach((id,i)=>{ if (state.items[id]) viewer.world.setItemIndex(state.items[id], i); });
    viewer.forceRedraw();
    if (FIT && !state.fitted && state.items.scene) {
      state.fitted=true;
      setTimeout(()=>{
        const item=state.items.scene;
        const r=item.imageToViewportRectangle(new OpenSeadragon.Rect(FIT[0],FIT[1],FIT[2],FIT[3]));
        viewer.viewport.fitBounds(r,true);
      },50);
    }
  }
  updateButtons(); requestDraw();
}
function setVisible(id,on){
  state.visible[id]=!!on;
  if ((id==='quads-ext' && on && state.extOpacity===0) ||
      (id==='quads-int' && on && state.intOpacity===0)) {
    if (id==='quads-ext') state.extOpacity=1; else state.intOpacity=1;
  }
  applyLayer(id); updateButtons(); requestDraw();
}

function resizeCanvas(){
  const c=$('artcanvas');
  c.width=window.innerWidth; c.height=window.innerHeight;
  requestDraw();
}
function requestDraw(){
  if (pendingDraw) return;
  pendingDraw=true;
  requestAnimationFrame(()=>{ pendingDraw=false; drawArt(); });
}
function worldToImage(wx,wy){
  // Image x runs with world -Y (TX0 is the world-Y tile); image y runs with
  // world -X (TY0 is the world-X tile). Matches centerCoords()/the HUD.
  return {x:((O-wy)/A-TX0)*PX, y:((O-wx)/A-TY0)*PX};
}
function worldToScreen(wx,wy){
  const item=state.items.scene; if(!item) return null;
  const p=worldToImage(wx,wy);
  const q=item.imageToViewerElementCoordinates(new OpenSeadragon.Point(p.x,p.y));
  return {x:q.x,y:q.y};
}
function cellCorners(g,cell){
  const m=g.mat;
  const p=(lx,ly)=>({x:m[0]*lx+m[1]*ly+m[4], y:m[2]*lx+m[3]*ly+m[5]});
  // image v=0 is local maxY, u=0 is local minX.
  return [p(cell.x0,cell.y1), p(cell.x1,cell.y1), p(cell.x0,cell.y0), p(cell.x1,cell.y0)];
}
function prepareGroups(){
  for (const g of state.groups) {
    g.cells.forEach(c=>{ c._world=cellCorners(g,c); });
  }
}
function ensureGroupImages(g){
  g.cells.forEach(c=>{
    if (!c._img) {
      const img=new Image();
      img.onload=requestDraw;
      img.src=c.img;
      c._img=img;
    }
  });
}
function drawArt(){
  const c=$('artcanvas'), ctx=c.getContext('2d');
  ctx.clearRect(0,0,c.width,c.height);
  if (!state.selected.size || !state.items.scene || state.intOpacity<=0) return;
  ctx.imageSmoothingEnabled=true;
  for (const g of state.groups) {
    if (!state.selected.has(g.id)) continue;
    for (const cell of g.cells) {
      const img=cell._img;
      if (!img || !img.complete || !img.naturalWidth) continue;
      const wc=cell._world;
      const p0=worldToScreen(wc[0].x,wc[0].y);
      const p1=worldToScreen(wc[1].x,wc[1].y);
      const p2=worldToScreen(wc[2].x,wc[2].y);
      if (!p0||!p1||!p2) continue;
      const w=img.naturalWidth, h=img.naturalHeight;
      // top edge p0->p1, left edge p0->p2
      ctx.save();
      ctx.globalAlpha=state.intOpacity;
      ctx.setTransform((p1.x-p0.x)/w, (p1.y-p0.y)/w,
                       (p2.x-p0.x)/h, (p2.y-p0.y)/h,
                       p0.x, p0.y);
      ctx.drawImage(img,0,0);
      ctx.restore();
    }
  }
}
function pointInQuad(px,py,q){
  let inside=false;
  for (let i=0,j=3;i<4;j=i++) {
    const xi=q[i].x, yi=q[i].y, xj=q[j].x, yj=q[j].y;
    if (((yi>py)!==(yj>py)) && (px < (xj-xi)*(py-yi)/(yj-yi)+xi)) inside=!inside;
  }
  return inside;
}
function centerCoords(){
  if (!viewer) return null;
  const item=state.items.scene; if(!item) return null;
  const c=viewer.viewport.getCenter(true);
  const p=item.viewportToImageCoordinates(c);
  const ix=p.x, iy=p.y;
  const tx=TX0+Math.floor(ix/PX), ty=TY0+Math.floor(iy/PX);
  const wy=O-(TX0+ix/PX)*A, wx=O-(TY0+iy/PX)*A;
  return {ix:ix,iy:iy,tx:tx,ty:ty,wx:wx,wy:wy};
}
function hitGroups(){
  const c=centerCoords(); if(!c) return [];
  return state.groups.filter(g=>g.cells.some(cell=>pointInQuad(c.wx,c.wy,cell._world)));
}
function renderHitChips(){
  const box=$('ghit'); const hits=hitGroups();
  box.innerHTML='';
  if (!state.groups.length) return;
  if (!hits.length) {
    const d=document.createElement('div'); d.className='none';
    d.textContent='No stock interior group at the crosshair.'; box.appendChild(d); return;
  }
  for (const g of hits) {
    const b=document.createElement('button');
    b.className='chip'+(state.selected.has(g.id)?' on':'');
    b.textContent=g.smo||('group '+g.grp);
    b.title=(g.wmo||'')+' · grp '+g.grp+' · z '+g.z[0].toFixed(0)+'..'+g.z[1].toFixed(0);
    b.addEventListener('click',()=>toggleGroup(g.id));
    box.appendChild(b);
  }
}
function scheduleHits(){
  if (hitTimer) clearTimeout(hitTimer);
  hitTimer=setTimeout(()=>{ hitTimer=null; renderHitChips(); }, 120);
}
function renderGroupList(){
  const box=$('glist'); box.innerHTML='';
  if (!state.groups.length) return;
  const f=state.groupFilter.toLowerCase();
  const rows=state.groups.filter(g=>{
    if (!f) return true;
    const wmo=(g.wmo||'').toLowerCase();
    return (g.smo||'').toLowerCase().includes(f) || wmo.includes(f) ||
           String(g.grp).includes(f) || String(g.id).includes(f);
  });
  const hits=new Set(hitGroups().map(g=>g.id));
  for (const g of rows) {
    const label=document.createElement('label');
    label.className='gitem'+(hits.has(g.id)?' hit':'');
    const cb=document.createElement('input');
    cb.type='checkbox'; cb.checked=state.selected.has(g.id); cb.dataset.id=g.id;
    cb.addEventListener('change',()=>toggleGroup(g.id));
    const span=document.createElement('span');
    const b=document.createElement('b'); b.textContent=g.smo||('group '+g.grp);
    const small=document.createElement('small');
    const wmo=(g.wmo||'').split('\\').pop().replace(/\.wmo$/i,'');
    small.textContent=wmo+' · grp '+g.grp+' · z '+g.z[0].toFixed(0)+'..'+g.z[1].toFixed(0)+
      ' · '+g.cells.length+' cell'+(g.cells.length===1?'':'s');
    span.appendChild(b); span.appendChild(small); label.appendChild(cb); label.appendChild(span);
    box.appendChild(label);
  }
  if (!rows.length) {
    const d=document.createElement('div'); d.className='none gsub';
    d.textContent='No groups match the filter.'; box.appendChild(d);
  }
}
function toggleGroup(id){
  if (state.selected.has(id)) state.selected.delete(id);
  else {
    const g=state.groups.find(x=>x.id===id);
    if (g) { state.selected.add(id); ensureGroupImages(g); }
  }
  renderHitChips(); renderGroupList(); requestDraw();
}
function setPanel(open){
  state.panelOpen=!!open;
  $('gpanel').classList.toggle('show', state.panelOpen);
  updateButtons();
}
function selectFromQuery(){
  const q=new URLSearchParams(location.search).get('xray');
  if (!q) return;
  for (const token of q.split(',')) {
    const t=token.trim();
    if (!t) continue;
    const g=state.groups.find(x=>String(x.id)===t || String(x.grp)===t ||
                               (x.smo||'').toLowerCase()===t.toLowerCase());
    if (g) { state.selected.add(g.id); ensureGroupImages(g); }
  }
}
function loadGroupArt(){
  if (!INSTANCE || !HAS_GROUPS) return;
  $('groupsrow').style.display='flex';
  fetch('interior-art.json',{cache:'no-store'})
    .then(r=>{ if(!r.ok) throw new Error('HTTP '+r.status); return r.json(); })
    .then(data=>{
      state.groups=data||[];
      prepareGroups();
      selectFromQuery();
      renderGroupList();
      renderHitChips();
      requestDraw();
      if (state.groups.length) setPanel(true);
      else {
        $('gerr').textContent='No stock interior art groups for this map.';
        $('groupsrow').style.display='none';
      }
    })
    .catch(e=>{
      $('gerr').textContent='Could not load interior-art.json: '+e.message;
      $('groupsrow').style.display='none';
    });
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
  if (!HAS_EXT) {
    $('t-quads-ext').parentElement.style.display='none';
    $('qextopacity').parentElement.style.display='none';
  }
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
        // A single aborted tile can surface here on a busy/slow connection even when
        // the DZI is valid. Keep the toggle enabled and only log the warning;
        // permanently disabling a layer that is already rendering is worse.
        console.error('layer load warning',l.file,ev);
        loaded++;
        if (loaded===LAYERS.length) applyAll();
      }
    });
  });
  function updateCenter(){
    if (!viewer) return;
    const c=centerCoords(); if(!c) return;
    $('hud').innerHTML='<b>map '+MAPID+'</b>  ADT <b>('+c.tx+', '+c.ty+')</b>\n'+
      'world <b>'+c.wx.toFixed(1)+', '+c.wy.toFixed(1)+'</b>\n'+
      'image '+c.ix.toFixed(0)+', '+c.iy.toFixed(0)+' px   zoom '+
      (viewer.viewport.getZoom(true)*100).toFixed(0)+'%';
    scheduleHits(); requestDraw();
  }
  viewer.addHandler('open',updateCenter);
  ['animation','animation-finish','resize','zoom','pan','canvas-scroll'].forEach(e=>viewer.addHandler(e,updateCenter));
  window.addEventListener('resize',()=>{ resizeCanvas(); updateCenter(); });
  window.addEventListener('keydown',e=>{ if(e.key==='f'||e.key==='F') viewer.viewport.goHome(); });

  function copyText(text){
    if (navigator.clipboard && window.isSecureContext) {
      navigator.clipboard.writeText(text).catch(()=>fallbackCopy(text));
    } else fallbackCopy(text);
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
  $('t-cross').addEventListener('click',()=>{ state.crosshair=!state.crosshair; updateButtons(); });
  $('t-groups').addEventListener('click',()=>setPanel(!state.panelOpen));
  $('gclose').addEventListener('click',()=>setPanel(false));
  $('gsearch').addEventListener('input',e=>{ state.groupFilter=e.target.value; renderGroupList(); });
  $('gclear').addEventListener('click',()=>{
    state.selected.clear(); renderGroupList(); renderHitChips(); requestDraw();
  });
  $('gat').addEventListener('click',()=>{
    for (const g of hitGroups()) {
      state.selected.add(g.id); ensureGroupImages(g);
    }
    renderGroupList(); renderHitChips(); requestDraw();
  });
  $('qextopacity').addEventListener('input',e=>{
    state.extOpacity=Number(e.target.value)/100;
    if (state.extOpacity>0 && !state.visible['quads-ext']) state.visible['quads-ext']=true;
    if (state.extOpacity===0) state.visible['quads-ext']=false;
    applyLayer('quads-ext'); updateButtons();
  });
  $('qintopacity').addEventListener('input',e=>{
    state.intOpacity=Number(e.target.value)/100;
    if (state.intOpacity>0 && !state.visible['quads-int']) state.visible['quads-int']=true;
    if (state.intOpacity===0) state.visible['quads-int']=false;
    applyLayer('quads-int'); updateButtons(); requestDraw();
  });
  $('copy').addEventListener('click',()=>{
    const c=centerCoords();
    const txt=c ? ('Map '+MAPID+' | ADT ('+c.tx+', '+c.ty+') | world '+
      c.wx.toFixed(1)+', '+c.wy.toFixed(1)+' | image '+
      c.ix.toFixed(0)+', '+c.iy.toFixed(0)+' px') : ('Map '+MAPID);
    copyText(txt);
    const b=$('copy'); b.textContent='Copied'; setTimeout(()=>{ b.textContent='Copy coords'; },1200);
  });
  $('reset').addEventListener('click',()=>{
    state.visible=Object.assign({},DEFAULTS);
    state.extOpacity=1; state.intOpacity=1;
    state.selected.clear();
    applyAll(); renderGroupList(); renderHitChips();
  });
  resizeCanvas();
  updateButtons();
  loadGroupArt();
}
(function(){
  const s=document.createElement('script');
  s.src='openseadragon/openseadragon.min.js';
  s.onload=initOSD;
  s.onerror=function(){ $('err').textContent='OpenSeadragon failed to load'; };
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
    ap.add_argument('--instance', type=int, default=-1,
                    help='show the per-group tunnel x-ray panel (default: auto by map id)')
    ap.add_argument('--has-ext', type=int, default=1,
                    help='the map has stock outdoor minimap tiles (default 1)')
    ap.add_argument('--has-groups', type=int, default=1,
                    help='interior-art.json exists and has groups (default 1)')
    args = ap.parse_args()

    fit = 'null'
    if args.fit:
        fit = '[' + ','.join(x.strip() for x in args.fit.split(',')) + ']'
    if args.instance >= 0:
        instance = args.instance
    else:
        # Continents keep the normal map viewer; instances, arenas and
        # battlegrounds get the per-group stock interior art panel.
        instance = 0 if args.map_id in (0, 1, 530, 571) else 1
    title = args.title or ('%s — map %d' % (args.name, args.map_id))
    html = (HTML.replace('@NAME@', args.name)
                .replace('@TITLE@', title)
                .replace('@MAPID@', str(args.map_id))
                .replace('@TX0@', str(args.tx0))
                .replace('@TY0@', str(args.ty0))
                .replace('@COUNT@', str(args.count))
                .replace('@FIT@', fit)
                .replace('@INSTANCE@', str(instance))
                .replace('@HAS_EXT@', '1' if args.has_ext else '0')
                .replace('@HAS_GROUPS@', '1' if args.has_groups else '0')
                .replace('@PX@', str(args.px)))
    out = args.out
    path = os.path.join(out, 'viewer.html')
    with open(path, 'w', encoding='utf-8') as f:
        f.write(html)
    # Keep the old URL working; both pages are the same unified viewer.
    with open(os.path.join(out, 'viewer-interiors.html'), 'w', encoding='utf-8') as f:
        f.write(html)
    print('wrote %s' % path)


if __name__ == '__main__':
    main()
