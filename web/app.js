'use strict';
import {spans,files,liveFile,disksFile,maxSnapshotBytes,historyLength,decodeLive,decodeHistory,decodeDisks,skipped as damagedRecords} from './codec.js';
import {age,colorNames} from './format.js';
import gripIcon from './icons/grip-vertical.svg' with {type:'text'};
import closeIcon from './icons/x.svg' with {type:'text'};
import {view,createTile,draw,metricKeys,metricLabel,isChart,clearInspection,queueInspection,setCharted} from './tiles.js';

const $=id=>document.getElementById(id);
const h=(tag,className,...children)=>{const e=document.createElement(tag);if(className)e.className=className;e.append(...children);return e;};
const range=$('range'),views=$('views'),statusLine=$('status'),grid=$('tiles'),edit=$('edit'),share=$('share'),nameInput=$('board-name');
const noProbes=Error('No devices have connected to this hub yet');
const MAX_BOARDS=64,MAX_TILES=128,MAX_BOARD_NAME=40;
const validHost=id=>typeof id==='string'&&/^[A-Za-z0-9_-][A-Za-z0-9_.-]{0,63}$/.test(id);
/* A view is one device, or a board: a named list of [host, metric] tiles.
 * The URL holds the view, so a link reproduces it; saved boards live in
 * localStorage. saved is the board's index there, or -1. */
let current=fromURL(),boards=loadBoards(),editing=false,adding=true,boardError='';
let hosts=[],hostsText='',tiles=[],pollTimer,requestController,refreshError=null,frame=0;
const sources=new Map();

function validTile(entry) {
  if(!Array.isArray(entry)||(entry.length!==2&&entry.length!==4))return false;
  const [host,metric,col,row]=entry;
  return validHost(host)&&metricKeys.includes(metric)&&(entry.length===2||
    Number.isInteger(col)&&col>=1&&col<=6&&Number.isInteger(row)&&row>=1&&row<=3*MAX_TILES);
}

function fromURL() {
  const query=new URLSearchParams(location.search);
  if(!query.has('board')){const host=query.get('host');return {kind:'device',host:validHost(host)?host:''};}
  const tiles=(query.get('tiles')||'').split(',',MAX_TILES).map(item=>item.split('~',3))
    .map(([host,metric,cell])=>/^\d+\.\d+$/.test(cell)?[host,metric,...cell.split('.').map(Number)]:[host,metric]).filter(validTile);
  return {kind:'board',name:query.get('board').slice(0,MAX_BOARD_NAME)||'board1',tiles,saved:-1};
}
function toURL() {
  const query=current.kind==='device'?(current.host?'?host='+encodeURIComponent(current.host):'?')
    :'?board='+encodeURIComponent(current.name)+'&tiles='+current.tiles.map(([host,metric,col,row])=>encodeURIComponent(host)+'~'+metric+(col?'~'+col+'.'+row:'')).join(',');
  history.replaceState(null,'',query);
}
function loadBoards() {
  try{
    const stored=JSON.parse(localStorage.getItem('quectop-boards'));
    if(Array.isArray(stored))return stored.slice(0,MAX_BOARDS).filter(b=>typeof b?.name==='string'&&b.name.trim()&&Array.isArray(b.tiles))
      .map(b=>({name:b.name.slice(0,MAX_BOARD_NAME),tiles:b.tiles.slice(0,MAX_TILES).filter(validTile)}));
  }catch{}
  return [];
}
function storeBoards(next) {
  try{localStorage.setItem('quectop-boards',JSON.stringify(next));}catch{return false;}
  boards=next;
  return true;
}
const hostName=id=>hosts.find(x=>x.id===id)?.name||id;

function source(id) {
  let src=sources.get(id);
  if(!src)sources.set(id,src={id,state:null,visible:[],history:null,skipped:0,disks:null,diskError:'',iface:0,changedAt:0,error:null,
    touch(){for(const t of tiles)if(t.src===src)t.update();queueDraw();}});
  return src;
}
function render() {
  clearInspection();
  const board=current.kind==='board',list=board?current.tiles:current.host?metricKeys.map(metric=>[current.host,metric]):[];
  const retained=new Map();
  for(const [host,metric] of list)retained.set(host,retained.get(host)||isChart(metric));
  for(const [id,src] of sources) {
    if(!retained.has(id))sources.delete(id);
    else if(!retained.get(id)){src.history=null;src.visible=[];src.pending=[];src.skipped=0;}
  }
  tiles=list.map(([host,metric,col,row])=>{const t=createTile(metric,source(host),board?hostName(host):'');t.optional=!board;pin(t,col,row);return t;});
  if(editing)tiles.forEach(decorate);
  grid.replaceChildren(...tiles.map(t=>t.el),...(editing?[addTile()]:[]));
  setCharted(tiles.filter(t=>isChart(t.metric)));
  edit.hidden=!board;$('delete').hidden=!editing||current.saved<0;
  edit.setAttribute('aria-pressed',String(editing));edit.setAttribute('aria-label',editing?(current.saved<0?'Save as new board':'Save board'):'Edit board');
  nameInput.hidden=!editing;$('title').hidden=editing;
  fillViews();updateAll();
}
function fillViews() {
  const option=(value,label)=>new Option(label,value),group=(label,...options)=>{const g=h('optgroup','',...options);g.label=label;return g;};
  const unsaved=current.kind==='board'&&current.saved<0;
  const add=option('new','New board');add.disabled=boards.length>=MAX_BOARDS;
  views.replaceChildren(group('Devices',...hosts.map(x=>option('h:'+x.id,x.name+(x.connected?'':' (offline)')))),
    group('Boards',...boards.map((b,i)=>option('b:'+i,b.name)),...(unsaved?[option('b:-1',current.name+' (unsaved)')]:[]),add));
  views.value=current.kind==='device'?'h:'+current.host:'b:'+current.saved;
}
function updateAll() {
  for(const t of tiles) {
    t.update();
    if(t.host){const issue=sourceIssue(t.src);t.host.textContent=hostName(t.src.id)+(issue?' · '+issue:'');t.host.title=issue?t.host.textContent:'';t.host.dataset.state=issue?'error':'ok';}
  }
  const src=current.kind==='device'&&sources.get(current.host),s=src&&src.state;
  $('host').textContent=current.kind==='board'?current.name:s?.hostname||hostName(current.host);
  document.title=($('host').textContent?$('host').textContent+' · ':'')+'quectop';
  $('detail').textContent=s?'Sample '+s.interval/1000+'s · '+src.visible.length+' windows · '+age(s.widths[view.index]*1000)+'/window'+(src.skipped?' · '+src.skipped+' damaged records skipped':''):'';
  status();queueDraw();
}
function quietTime(src){return Date.now()-src.changedAt;}
function stale(src) {
  const {interval,publish}=src.state;
  return quietTime(src)>Math.max(90000,3*Math.ceil(publish/interval)*interval);
}
function sourceIssue(src) {
  if(refreshError||src.error||src.diskError)return 'refresh failed';
  if(!hosts.find(x=>x.id===src.id)?.connected)return 'offline';
  if(!src.state)return 'awaiting data';
  return stale(src)?'stale':'';
}
function status() {
  if(boardError) {
    statusLine.dataset.state='error';statusLine.textContent=boardError;statusLine.title='';return;
  }
  if(refreshError) {
    const cached=[...sources.values()].some(src=>src.state);
    statusLine.dataset.state='error';
    statusLine.textContent=cached?'Refresh failed · showing last snapshot':refreshError===noProbes?refreshError.message:'Cannot load data from quectop-hub';
    statusLine.title=refreshError.message;
    return;
  }
  if(current.kind==='board') {
    const issues=[],counts=new Map();
    let cached=false;
    for(const src of sources.values()) {
      const issue=sourceIssue(src);
      if(!issue)continue;
      counts.set(issue,(counts.get(issue)||0)+1);
      const reason=src.error?.message||src.diskError;
      issues.push(hostName(src.id)+' · '+issue+(reason?' · '+reason:''));
      cached||=Boolean(src.state);
    }
    const size=sources.size,down=issues.length;
    statusLine.dataset.state=down?'error':size?'ok':'';
    statusLine.textContent=size?size+(size===1?' device':' devices')+(down?' · '+[...counts].map(([label,count])=>count+' '+label).join(', ')+(cached?' · showing last data':''):' connected'):editing?'Add tiles below':'Empty board';
    statusLine.title=issues.join('\n');return;
  }
  const src=sources.get(current.host),s=src?.state,connected=hosts.find(x=>x.id===current.host)?.connected;
  if(src?.error){statusLine.dataset.state='error';statusLine.textContent=s?'Refresh failed · showing last snapshot':'Cannot load data from quectop-hub';statusLine.title=src.error.message;return;}
  if(!s){statusLine.dataset.state='';statusLine.textContent='Loading data';statusLine.title='';return;}
  /* Freshness uses only the browser clock: the device clock may be wrong. */
  const delayed=stale(src);
  statusLine.dataset.state=!connected||delayed?'error':'ok';
  statusLine.textContent='uptime: '+age(s.uptime)+(!connected?' · offline':delayed?' · no new data for '+age(quietTime(src)):' · connected');
  statusLine.title=!connected?'The hub has no connection from this device.':delayed?'The device is connected, but no new data has arrived.':'';
}
function redraw() {
  const count=Math.max(2,...[...sources.values()].map(src=>Math.max(src.state?.cores||0,src.state?.frequencies||0)));
  if(view.colors?.theme!==theme+font||view.colors.palette.length!==count) {
    const style=getComputedStyle(document.documentElement),lightness=style.getPropertyValue('--series-lightness').trim();
    view.colors={theme:theme+font,font:style.getPropertyValue('--font').trim(),grid:style.getPropertyValue('--grid'),muted:style.getPropertyValue('--muted'),
      palette:Array.from({length:count},(_,k)=>k<colorNames.length?style.getPropertyValue(colorNames[k]).trim():'hsl('+((k*137.508)%360).toFixed(1)+' 55% '+lightness+')')};
  }
  for(const t of tiles)if(isChart(t.metric))draw(t);
  queueInspection();
}
function queueDraw(){if(!frame)frame=requestAnimationFrame(()=>{frame=0;redraw();});}

async function request(name,signal) {
  const response=await fetch(name,{cache:'no-cache',signal:AbortSignal.any([signal,AbortSignal.timeout(10000)])});
  if(!response.ok)throw Error(name+': HTTP '+response.status);
  return response;
}
async function download(id,name,signal,expected) {
  const limit=expected??maxSnapshotBytes;
  const invalid=()=>Error(name+' has an unexpected length');
  if(limit>256*1024*1024)throw invalid();
  const response=await request('h/'+encodeURIComponent(id)+'/'+name,signal);
  const length=response.type==='cors'||response.headers.has('Content-Encoding')?null:response.headers.get('Content-Length');
  const announced=length===null?null:Number(length);
  if(announced!==null&&(!Number.isSafeInteger(announced)||announced<0||announced>limit||expected!==undefined&&announced!==expected)){
    void response.body?.cancel().catch(()=>{});
    throw invalid();
  }
  const reader=response.body?.getReader();
  if(!reader)throw invalid();
  let data=new Uint8Array(announced??0),size=0;
  try {
    for(;;) {
      const {done,value}=await reader.read();
      if(done)break;
      const end=size+value.byteLength;
      if(end>limit){void reader.cancel().catch(()=>{});throw invalid();}
      if(end>data.length){const grown=new Uint8Array(Math.min(limit,Math.max(end,data.length*2)));grown.set(data);data=grown;}
      data.set(value,size);size=end;
    }
  }finally{reader.releaseLock();}
  signal.throwIfAborted();
  if(expected!==undefined&&size!==expected)throw invalid();
  return size===data.length?data.buffer:data.buffer.slice(0,size);
}
/* Hub lines: id, connected (0/1), hostname. */
async function loadHosts(signal) {
  const text=await (await request('hosts.txt',signal)).text();
  signal.throwIfAborted();
  if(text===hostsText)return;
  hostsText=text;
  hosts=text.split('\n').map(line=>{const[id,connected,name]=line.split('\t');return{id,connected:connected==='1',name:name||id};}).filter(x=>validHost(x.id));
  if(current.kind==='device'&&!hosts.some(x=>x.id===current.host)){
    const pick=hosts.find(x=>x.connected)||hosts[0];
    if(pick){current.host=pick.id;toURL();render();return;}
  }
  fillViews();
}
/* Fetches what the view's tiles of one device need: the live snapshot,
 * history only for charts, capacities only for a Disks tile. */
async function load(src,index,need,signal) {
  const next=decodeLive(await download(src.id,liveFile,signal));next.index=index;
  if(need.chart) {
    const active=next.active[index],key=[index,next.points,next.interval,next.cores,next.idBits,next.frequencies,next.frequencyBits,next.interfaces,next.metadataHash,next.committed[index]].join(':');
    if(src.history?.key!==key){const data=decodeHistory(await download(src.id,files[index],signal,historyLength(next,index)),index,next);src.history={key,data};src.skipped=damagedRecords;}
    const cutoff=next.latestTime-spans[index]*1000,width=next.widths[index]*1000;
    const recent=index===0?next.pending.slice():[];if(active)recent.push(active);
    const first=recent.length?recent[0].start:Infinity,newest=active?.start??next.latestTime,rows=[];
    for(const r of src.history.data)if(r.start+width>cutoff&&r.start<first&&r.start<=newest)rows.push(r);
    for(const r of recent)if(r.start+width>cutoff)rows.push(r);
    src.pending=rows.length>next.points?rows.slice(-next.points):rows;
  }else src.pending=[];
  if(need.disks&&(!src.disks||src.disks.generated!==next.disksGenerated)){
    try{const snapshot=decodeDisks(await download(src.id,disksFile,signal));if(snapshot.generated>=next.disksGenerated){src.disks=snapshot;src.diskError='';}}
    catch(error){signal.throwIfAborted();src.diskError=error.message;}
  }
  if(!need.disks||src.disks?.generated===next.disksGenerated)src.diskError='';
  next.active=next.pending=null; // visible rows keep the buckets the charts use
  if(next.generated!==src.state?.generated)src.changedAt=Date.now();
  if(src.iface>=next.interfaces)src.iface=0;
  return next;
}
async function refresh() {
  clearTimeout(pollTimer);if(document.hidden||requestController)return;
  requestController=new AbortController();const {signal}=requestController;
  const index=Number(range.value);let failure=null;
  try {
    try{await loadHosts(signal);refreshError=null;}
    catch(error){if(signal.aborted)return;refreshError=error;throw error;}
    if(current.kind==='device'&&!current.host)throw noProbes;
    const needs=new Map();
    for(const t of tiles){const need=needs.get(t.src)||{chart:false,disks:false};need.chart||=isChart(t.metric);need.disks||=t.metric==='disks';needs.set(t.src,need);}
    const loaded=[];
    for(const [src,need] of needs){try{loaded.push([src,await load(src,index,need,signal)]);src.error=null;}catch(error){if(signal.aborted)return;src.error=error;failure||=error;}}
    if(index!==Number(range.value))return;
    if(failure&&index!==view.index){range.value=String(view.index);return;}
    view.index=index;
    for(const [src,next] of loaded){src.state=next;src.visible=src.pending||[];}
  }catch(error){failure=error;if(error===noProbes)refreshError=error;if(index!==view.index)range.value=String(view.index);}
  finally{
    requestController=null;
    if(signal.aborted)refresh();
    else{updateAll();if(!document.hidden)pollTimer=setTimeout(refresh,index!==Number(range.value)?0:15000);}
  }
}

/* Edit mode: tiles gain a drag handle (arrow keys also move it) and a remove
 * button; an extra tile adds new ones. Changes apply at once; save stores. */
function decorate(t) {
  const handle=h('button','tile-drag'),remove=h('button','tile-remove');handle.innerHTML=gripIcon;remove.innerHTML=closeIcon;
  handle.setAttribute('aria-label','Move '+metricLabel(t.metric)+' of '+hostName(t.src.id)+'; use the arrow keys');
  remove.setAttribute('aria-label','Remove '+metricLabel(t.metric)+' of '+hostName(t.src.id));
  t.el.prepend(h('div','tile-edit',handle,remove));t.el.classList.add('editing');
  remove.addEventListener('click',()=>{current.tiles.splice(tiles.indexOf(t),1);changed();});
  handle.addEventListener('keydown',event=>{
    const step={ArrowLeft:-1,ArrowUp:-1,ArrowRight:1,ArrowDown:1}[event.key];
    if(!step)return;event.preventDefault();move(tiles.indexOf(t),tiles.indexOf(t)+step);handle.focus();
  });
  handle.addEventListener('pointerdown',event=>{
    event.preventDefault();
    const capture=()=>{try{handle.setPointerCapture(event.pointerId);}catch{}};capture();
    /* The tile's slot stays as a dashed placeholder; a copy follows the pointer. */
    const box=t.el.getBoundingClientRect(),dx=event.clientX-box.left,dy=event.clientY-box.top,lifted=liftedCopy(t.el,box);
    t.el.classList.add('dragging');
    let last=[-99,-99];
    const place=e=>{lifted.style.left=e.clientX-dx+'px';lifted.style.top=e.clientY-dy+'px';};
    const follow=e=>{
      if(e.pointerId!==event.pointerId)return;
      place(e);
      if(performance.now()<settled||Math.hypot(e.clientX-last[0],e.clientY-last[1])<24)return;
      const over=document.elementFromPoint(e.clientX,e.clientY)?.closest('.tile'),to=tiles.findIndex(x=>x.el===over),i=tiles.indexOf(t);
      if(to>=0){if(tiles[to]!==t){unpin(i);move(i,to);capture();last=[e.clientX,e.clientY];}return;}
      const cell=freeCell(t,e.clientX,e.clientY);
      if(cell){animate(()=>{current.tiles[i]=[...current.tiles[i].slice(0,2),...cell];pin(t,...cell);});last=[e.clientX,e.clientY];}
    };
    // Re-inserting tiles drops pointer capture: listen on the window, and recapture after each move.
    const stop=e=>{
      if(e.pointerId!==event.pointerId)return;
      removeEventListener('pointermove',follow);removeEventListener('pointerup',stop);removeEventListener('pointercancel',stop);
      const from=lifted.getBoundingClientRect(),to=t.el.getBoundingClientRect();lifted.remove();t.el.classList.remove('dragging');
      slide(t.el,from.left-to.left,from.top-to.top);
    };
    addEventListener('pointermove',follow);addEventListener('pointerup',stop);addEventListener('pointercancel',stop);
  });
}
/* Pins: on wide screens a tile dropped into an empty cell keeps that cell
 * (grid lines col, row); other tiles pack around it. Narrower screens use
 * other spans, so there every tile packs in list order. */
const wide=matchMedia('(min-width: 1281px)');
function pin(t,col,row){t.el.classList.toggle('pinned',Boolean(col));if(col){t.el.style.setProperty('--col',col);t.el.style.setProperty('--row',row);}}
function unpin(i){if(current.tiles[i].length>2){current.tiles[i]=current.tiles[i].slice(0,2);pin(tiles[i]);}}
/* Track start offsets of a grid axis, from its resolved track sizes. */
function lines(sizes,start,gap){const at=[];for(const size of sizes.split(' ').map(parseFloat)){at.push(start);start+=size+gap;}at.push(start);return at;}
/* The free cell under the pointer that fits tile t, as [col, row], or null. */
function freeCell(t,x,y) {
  if(!wide.matches)return null;
  const style=getComputedStyle(grid),area=grid.getBoundingClientRect(),gap=parseFloat(style.columnGap);
  const cols=lines(style.gridTemplateColumns,area.left,gap),rows=lines(style.gridTemplateRows,area.top,gap);
  const index=(at,v)=>{let i=0;while(i<at.length-2&&v>=at[i+1]-gap/2)i++;return i;};
  if(x<area.left||x>area.right||y<area.top)return null;
  const span=t.el.classList.contains('summary')?[1,1]:[2,3],col=index(cols,x),row=y>area.bottom?rows.length-1:index(rows,y);
  if(col+span[0]>cols.length-1)return null;
  for(const other of grid.children){
    if(other===t.el)continue;
    const r=other.getBoundingClientRect(),c0=index(cols,r.left+1),r0=index(rows,r.top+1),c1=index(cols,r.right-1),r1=index(rows,r.bottom-1);
    if(c0<col+span[0]&&col<=c1&&r0<row+span[1]&&row<=r1)return null;
  }
  return [col+1,row+1];
}
function liftedCopy(el,box) {
  const copy=el.cloneNode(true),sources=el.querySelectorAll('canvas');
  copy.querySelectorAll('canvas').forEach((canvas,i)=>{canvas.width=sources[i].width;canvas.height=sources[i].height;canvas.getContext('2d').drawImage(sources[i],0,0);});
  copy.classList.add('lifted');
  Object.assign(copy.style,{left:box.left+'px',top:box.top+'px',width:box.width+'px',height:box.height+'px'});
  document.body.append(copy);return copy;
}
/* Moves animate: each tile slides from its old position to its new one. */
const still=matchMedia('(prefers-reduced-motion: reduce)').matches;
let settled=0;
function slide(el,x,y){if(!still&&(x||y))el.animate([{transform:'translate('+x+'px,'+y+'px)'},{transform:'none'}],{duration:180,easing:'ease-out'});}
/* Applies a layout change, then slides every tile from its old position. */
function animate(change) {
  const before=new Map(tiles.map(t=>[t.el,t.el.getBoundingClientRect()]));
  change();
  for(const [el,old] of before){const now=el.getBoundingClientRect();slide(el,old.left-now.left,old.top-now.top);}
  // Ignore the pointer while tiles slide, so a tile moving under it does not swap back.
  settled=performance.now()+180;
  toURL();queueDraw();
}
function move(from,to) {
  if(to<0||to>=tiles.length||from===to)return;
  animate(()=>{
    tiles.splice(to,0,...tiles.splice(from,1));current.tiles.splice(to,0,...current.tiles.splice(from,1));
    grid.replaceChildren(...tiles.map(t=>t.el),...[...grid.children].filter(e=>e.classList.contains('add-tile')));
  });
}
let addHost='',addMetric=metricKeys[0];
function addTile() {
  if(!adding||current.tiles.length>=MAX_TILES)return addTileClosed();
  const host=h('select'),metric=h('select'),add=h('button','','Add tile');
  host.setAttribute('aria-label','Device');metric.setAttribute('aria-label','Metric');
  host.append(...hosts.map(x=>new Option(x.name,x.id)));metric.append(...metricKeys.map(key=>new Option(metricLabel(key),key)));
  host.value=addHost||hosts[0]?.id||'';metric.value=addMetric;
  add.addEventListener('click',()=>{const tile=[host.value,metric.value];if(!validTile(tile))return;addHost=host.value;addMetric=metric.value;current.tiles.push(tile);changed();});
  const cancel=h('button','','Cancel');
  cancel.addEventListener('click',()=>{adding=false;render();});
  return h('div','tile add-tile',host,metric,add,cancel);
}
/* With the form closed, the last tile reopens it or saves the board. */
function addTileClosed() {
  const open=h('button','','Add tile'),done=h('button','',current.saved<0?'Save as new':'Save and exit');
  open.disabled=current.tiles.length>=MAX_TILES;
  if(open.disabled)open.title='Board tile limit reached';
  open.addEventListener('click',()=>{adding=true;render();});
  done.addEventListener('click',save);
  return h('div','tile add-tile',open,done);
}
function changed(){requestController?.abort();toURL();render();refresh();}
function boardFailure(message){boardError=message;status();}
function save() {
  if(current.saved<0&&boards.length>=MAX_BOARDS){
    boardFailure('Board limit reached. Delete a saved board to save this one.');return;
  }
  const name=nameInput.value.trim().slice(0,MAX_BOARD_NAME)||current.name;
  let chosen=name;
  for(let i=2;boards.some((b,index)=>index!==current.saved&&b.name===chosen);i++){
    const suffix=' ('+i+')';chosen=name.slice(0,MAX_BOARD_NAME-suffix.length)+suffix;
  }
  const next=boards.slice(),index=current.saved<0?next.length:current.saved;
  next[index]={name:chosen,tiles:current.tiles.map(entry=>[...entry])};
  if(!storeBoards(next)) {
    current.name=name;toURL();fillViews();
    boardFailure('Could not save board. Copy the link to keep your draft.');
    return;
  }
  current.name=chosen;current.saved=index;boardError='';editing=false;toURL();render();
}
function nextName(){let n=1;while(boards.some(b=>b.name==='board'+n))n++;return 'board'+n;}
function beginEditing(){nameInput.value=current.name;editing=adding=true;}
edit.addEventListener('click',()=>{if(editing)save();else{beginEditing();render();nameInput.focus();}});
nameInput.addEventListener('keydown',event=>{if(event.key==='Enter')save();});
$('delete').addEventListener('click',()=>{
  if(current.saved<0)return;
  if(!confirm('Delete '+boards[current.saved].name+'?'))return;
  if(!storeBoards(boards.filter((_,index)=>index!==current.saved))){
    boardFailure('Could not delete board. Try again.');return;
  }
  boardError='';editing=false;current={kind:'device',host:hosts.find(x=>x.connected)?.id||hosts[0]?.id||''};changed();
});
share.addEventListener('click',async()=>{
  let copied=true;try{await navigator.clipboard.writeText(location.href);}catch{copied=false;}
  share.dataset.copied=String(copied);share.title=copied?'Link copied':'Copy failed; use the address bar';
  setTimeout(()=>{delete share.dataset.copied;share.title='Copy link to this view';},2000);
});
views.addEventListener('change',()=>{
  const value=views.value;boardError='';editing=false;
  if(value==='new'){current={kind:'board',name:nextName(),tiles:[],saved:-1};beginEditing();}
  else if(value.startsWith('h:'))current={kind:'device',host:value.slice(2)};
  else if(value!=='b:-1'){const i=Number(value.slice(2));current={kind:'board',name:boards[i].name,tiles:boards[i].tiles.map(entry=>[...entry]),saved:i};}
  changed();
});
range.addEventListener('change',()=>{clearInspection();refresh();});
/* Font: the system monospace (default) or the system sans; kept per browser. */
const fontSelect=$('font');let font;try{font=localStorage.getItem('quectop-font');}catch{}
if(font!=='sans')font='mono';
function setFont(){document.documentElement.dataset.font=fontSelect.value=font;queueDraw();}
fontSelect.addEventListener('change',()=>{font=fontSelect.value;try{localStorage.setItem('quectop-font',font);}catch{}setFont();});
let theme;try{theme=localStorage.getItem('quectop-theme');}catch{}
if(theme!=='dark'&&theme!=='light')theme=matchMedia('(prefers-color-scheme: dark)').matches?'dark':'light';
function setTheme(){document.documentElement.dataset.theme=theme;$('theme').setAttribute('aria-label','Use '+(theme==='light'?'dark':'light')+' theme');queueDraw();}
setFont();setTheme();$('theme').addEventListener('click',()=>{theme=theme==='light'?'dark':'light';try{localStorage.setItem('quectop-theme',theme);}catch{}setTheme();});
new ResizeObserver(queueDraw).observe(grid);
document.addEventListener('visibilitychange',()=>{if(document.hidden)clearTimeout(pollTimer);else refresh();});
render();refresh();
