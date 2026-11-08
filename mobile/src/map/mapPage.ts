// mapPage.ts - the map screen's HTML: MapLibre GL + the event's own vector tiles (one .pmtiles file the
// organizer put on Logos Storage; docs/adr/0002). Everything is inlined: the page never touches the
// network. The tiles arrive from React Native in base64 chunks (window.__crema.addChunk / mapReady);
// until then the cafés show on a plain background. No text layers: café names are HTML markers, so
// the style needs no glyph files. Map data (c) OpenStreetMap contributors (ODbL).
import { MAPLIBRE_CSS, MAPLIBRE_JS, PMTILES_JS } from "./vendor.generated";

export interface MapCafe { n: string; a: string; lat: number; lon: number; v: boolean }

const C = { bg: "#1d1410", land: "#2a1d16", park: "#2e3320", wood: "#283019", water: "#1f2f3a", building: "#3a2a20",
  road: "#6b4f3f", major: "#8a6a50", path: "#4d392d", rail: "#4a3a30", line: "#2a1d16", crema: "#d9a066", text: "#f3e7da" };

// OpenMapTiles schema (tilemaker's profile)
export function mapStyle(withTiles: boolean, maxzoom: number) {
  const layers: any[] = [{ id: "bg", type: "background", paint: { "background-color": withTiles ? C.land : C.bg } }];
  if (withTiles) {
    const src = "crema";
    layers.push(
      { id: "landcover-wood", type: "fill", source: src, "source-layer": "landcover", filter: ["in", ["get", "class"], ["literal", ["wood", "forest"]]], paint: { "fill-color": C.wood } },
      { id: "landcover-grass", type: "fill", source: src, "source-layer": "landcover", filter: ["in", ["get", "class"], ["literal", ["grass", "farmland", "meadow"]]], paint: { "fill-color": C.park, "fill-opacity": 0.7 } },
      { id: "park", type: "fill", source: src, "source-layer": "park", paint: { "fill-color": C.park } },
      { id: "landuse-park", type: "fill", source: src, "source-layer": "landuse", filter: ["in", ["get", "class"], ["literal", ["park", "cemetery", "pitch", "playground"]]], paint: { "fill-color": C.park, "fill-opacity": 0.8 } },
      { id: "water", type: "fill", source: src, "source-layer": "water", paint: { "fill-color": C.water } },
      { id: "waterway", type: "line", source: src, "source-layer": "waterway", paint: { "line-color": C.water, "line-width": 2 } },
      { id: "building", type: "fill", source: src, "source-layer": "building", minzoom: 13, paint: { "fill-color": C.building, "fill-outline-color": C.line } },
      { id: "path", type: "line", source: src, "source-layer": "transportation", filter: ["in", ["get", "class"], ["literal", ["path", "track"]]], minzoom: 14,
        paint: { "line-color": C.path, "line-width": 1, "line-dasharray": [2, 1] } },
      { id: "rail", type: "line", source: src, "source-layer": "transportation", filter: ["in", ["get", "class"], ["literal", ["rail", "transit"]]],
        paint: { "line-color": C.rail, "line-width": 1.5, "line-dasharray": [3, 2] } },
      { id: "road-minor", type: "line", source: src, "source-layer": "transportation", filter: ["in", ["get", "class"], ["literal", ["minor", "service", "tertiary"]]],
        layout: { "line-cap": "round", "line-join": "round" }, paint: { "line-color": C.road, "line-width": ["interpolate", ["linear"], ["zoom"], 12, 0.5, 16, 4, 18, 10] } },
      { id: "road-major", type: "line", source: src, "source-layer": "transportation", filter: ["in", ["get", "class"], ["literal", ["primary", "secondary", "trunk", "motorway"]]],
        layout: { "line-cap": "round", "line-join": "round" }, paint: { "line-color": C.major, "line-width": ["interpolate", ["linear"], ["zoom"], 10, 1, 16, 7, 18, 16] } },
    );
  }
  return {
    version: 8,
    sources: withTiles ? { crema: { type: "vector", url: "pmtiles://crema", maxzoom, attribution: "© OpenStreetMap contributors" } } : {},
    layers,
  };
}

export function mapPage(cafes: MapCafe[], maxzoom = 16): string {
  return `<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1">
<style>${MAPLIBRE_CSS}
html,body,#m{margin:0;height:100%;background:${C.bg}}
.cafe{width:18px;height:18px;border-radius:50%;border:2px solid ${C.line};box-shadow:0 1px 4px #0008;cursor:pointer}
.maplibregl-popup-content{background:${C.land};color:${C.text};border-radius:10px;font:14px sans-serif}
.maplibregl-popup-tip{border-top-color:${C.land}!important;border-bottom-color:${C.land}!important}
.maplibregl-ctrl-attrib{background:#0008!important;color:#b59c86}.maplibregl-ctrl-attrib a{color:#b59c86}
#note{position:absolute;left:8px;top:8px;padding:4px 8px;border-radius:8px;background:#0009;color:#b59c86;font:12px sans-serif}
</style></head><body><div id="m"></div><div id="note"></div>
<script>${MAPLIBRE_JS}</script><script>${PMTILES_JS}</script><script>
var cafes=${JSON.stringify(cafes)};
var STYLE_BASE=${JSON.stringify(mapStyle(false, maxzoom))};
var STYLE_TILES=${JSON.stringify(mapStyle(true, maxzoom))};
var m=new maplibregl.Map({container:'m',style:STYLE_BASE,center:[16.608,49.195],zoom:14,attributionControl:{compact:true},maxZoom:19});
function note(t){var n=document.getElementById('note');n.textContent=t||'';n.style.display=t?'block':'none';}
function post(o){try{window.ReactNativeWebView.postMessage(JSON.stringify(o));}catch(e){}}
var b=null,pts=[];
cafes.forEach(function(c){
  var el=document.createElement('div');el.className='cafe';el.style.background=c.v?'${C.crema}':'#6b4f3f';
  var p=document.createElement('div');var t=document.createElement('b');t.textContent=c.n;p.appendChild(t);
  if(c.a){p.appendChild(document.createElement('br'));p.appendChild(document.createTextNode(c.a));}
  new maplibregl.Marker({element:el}).setLngLat([c.lon,c.lat]).setPopup(new maplibregl.Popup({offset:12}).setDOMContent(p)).addTo(m);
  pts.push([c.lon,c.lat]);
});
if(pts.length){var bb=pts.reduce(function(a,p){return[[Math.min(a[0][0],p[0]),Math.min(a[0][1],p[1])],[Math.max(a[1][0],p[0]),Math.max(a[1][1],p[1])]];},[pts[0],pts[0]]);
  m.fitBounds(bb,{padding:40,maxZoom:16,duration:0});}
// progressive (0.3): the map file arrives as Storage pieces in file order; a PMTiles prefix holds the
// directories and the coarse zooms, so each zoom shows as soon as its bytes are in
var P=null,have=0,buf=null,availZ=-1,proto=null;
function styleUpTo(z){var st=JSON.parse(JSON.stringify(STYLE_TILES));
  st.sources.crema={type:'vector',tiles:['pmtiles://crema/{z}/{x}/{y}'],minzoom:P.minzoom,maxzoom:z,attribution:'© OpenStreetMap contributors'};return st;}
function refresh(){
  if(!P||have<P.dirEnd)return;
  var z=-1;P.zoomEnds.forEach(function(e){if(e[1]<=have&&e[0]>z)z=e[0];});
  if(z<0||z<=availZ)return;
  if(!proto){
    var src={getKey:function(){return 'crema';},getBytes:function(o,l){
      if(o+l<=have)return Promise.resolve({data:buf.buffer.slice(o,o+l)});
      if(o===0)return Promise.resolve({data:buf.buffer.slice(0,have)});   // the first read asks for 16 KiB up front
      return Promise.reject(new Error('not here yet'));}};
    proto=new pmtiles.Protocol();maplibregl.addProtocol('pmtiles',proto.tile);proto.add(new pmtiles.PMTiles(src));
  }
  var first=availZ<0;availZ=z;
  if(first){m.setStyle(styleUpTo(z));}
  else{   // swapping the style again (a changed source) leaves the map blank: raise maxzoom on the live source
    var s=m.getSource('crema');
    if(s){s.maxzoom=z;if(s._options)s._options.maxzoom=z;s.setTiles(['pmtiles://crema/{z}/{x}/{y}']);}
  }
  post({t:'zoom',z:z,have:have});
}
// the event's tiles, from React Native in base64 pieces
var parts=[];
window.__crema={
  note:note,
  addChunk:function(s){parts.push(s);},
  begin:function(meta){P=meta;buf=new Uint8Array(meta.size);have=0;availZ=-1;},
  addPiece:function(b64){
    try{var bin=atob(b64);if(!P||have+bin.length>buf.length)return;for(var i=0;i<bin.length;i++)buf[have+i]=bin.charCodeAt(i);have+=bin.length;refresh();}
    catch(e){note('Map failed: '+e);post({t:'map',ok:false,error:String(e)});}
  },
  mapReady:function(){
    try{
      var bin=atob(parts.join(''));parts=[];var buf=new Uint8Array(bin.length);for(var i=0;i<bin.length;i++)buf[i]=bin.charCodeAt(i);
      var src={getKey:function(){return 'crema';},getBytes:function(o,l){return Promise.resolve({data:buf.buffer.slice(o,o+l)});}};
      var proto=new pmtiles.Protocol();maplibregl.addProtocol('pmtiles',proto.tile);proto.add(new pmtiles.PMTiles(src));
      m.setStyle(STYLE_TILES);note('');post({t:'map',ok:true,bytes:buf.length});
    }catch(e){note('Map failed: '+e);post({t:'map',ok:false,error:String(e)});}
  }
};
post({t:'ready'});
</script></body></html>`;
}
