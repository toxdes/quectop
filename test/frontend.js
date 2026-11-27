'use strict';
// Exercise the shipped app as a black box, including minified release assets.
const fs=require('fs'),vm=require('vm');
const fixture=process.argv.at(-1),assets=process.env.QUECTOP_TEST_ASSETS||'web';
if(!fixture||fixture.endsWith('.js'))throw Error('Usage: bun test/frontend.js FIXTURE_DIRECTORY');
const html=fs.readFileSync(assets+'/index.html','utf8');
let source=fs.readFileSync(assets+'/app.js','utf8');
if(assets==='web'){
  const bundle=await Bun.build({entrypoints:[assets+'/app.js'],target:'browser',format:'esm',banner:'"use strict";',write:false});
  if(!bundle.success)throw new AggregateError(bundle.logs,'Frontend bundle failed');
  source=await bundle.outputs[0].text();
}
const ids=[...html.matchAll(/id="([^"]+)"/g)].map(m=>m[1]);
let strokes=0,failNext=false,failHosts=false,failStorage=false,wrongLength='',timer;
const frames=[],noop=()=>{},context2d=new Proxy({},{get:(_,key)=>key==='stroke'?()=>strokes++:key==='measureText'?value=>({width:String(value).length*8.4}):noop,set:()=>true});
// A minimal element: real child lists, listeners and attributes.
const node=()=>({value:'',dataset:{},style:{setProperty:noop},textContent:'',className:'',children:[],listeners:{},attributes:{},offsetWidth:210,offsetHeight:180,
  classList:{add:noop,remove:noop,toggle:noop},focus(){document.activeElement=this;},setPointerCapture:noop,
  addEventListener(name,fn){this.listeners[name]=fn;},removeEventListener:noop,setAttribute(name,value){this.attributes[name]=value;},
  replaceChildren(...children){this.children=children;},append(...children){this.children=this.children.concat(children);},prepend(...children){this.children=children.concat(this.children);},
  getBoundingClientRect:()=>({width:400,height:220,left:0,top:0}),getContext:()=>context2d});
const nodes=Object.fromEntries(ids.map(id=>[id,node()])),requests=[],hostRequests=[],windowListeners={},stored={};
const location={search:''};
let hostsText='fixture\t1\tfixture-host\nother\t0\tother-host\n';
nodes.range.value='1';
const document={hidden:false,documentElement:node(),listeners:{},createElement:node,getElementById(id){if(!nodes[id])throw Error('Missing element '+id);return nodes[id];},addEventListener(name,fn){this.listeners[name]=fn;}};
class Option{constructor(text,value){Object.assign(this,node(),{textContent:text,value});}}
const context=vm.createContext({document,console,location,Option,URLSearchParams,addEventListener(name,fn){windowListeners[name]=fn;},DataView,Uint8Array,Date,Math,Number,Error,Promise,AbortSignal,AbortController,TextDecoder,
  history:{replaceState(_,__,url){location.search=url;}},navigator:{clipboard:{writeText:async()=>{}}},confirm:()=>true,
  devicePixelRatio:1,getComputedStyle:()=>({getPropertyValue:()=> '#567890'}),
  requestAnimationFrame(fn){frames.push(fn);return 1;},clearTimeout(){timer=null;},setTimeout(fn){timer=fn;return 1;},
  ResizeObserver:class{observe(){}},matchMedia:()=>({matches:false}),localStorage:{getItem:key=>stored[key]??null,setItem(key,value){if(failStorage)throw Error('storage unavailable');stored[key]=value;}},
  async fetch(name){
    if(name==='hosts.txt'){if(failHosts)throw Error('index unreachable');return new Response(hostsText);}
    hostRequests.push(name);name=name.replace(/^h\/fixture\//,'');requests.push(name);
    if(failNext){failNext=false;throw Error('simulated disconnect');}
    const bytes=fs.readFileSync(fixture+'/'+name);
    return name===wrongLength?new Response(Buffer.concat([bytes,Buffer.from([0])])):new Response(bytes,{headers:{'Content-Length':String(bytes.length)}});}
});
const assert=(value,message)=>{if(!value)throw Error(message);};
async function settle(){await Bun.sleep(5);while(frames.length)frames.shift()();}
async function poll(){const fn=timer;assert(fn,'refresh scheduled');await fn();await settle();}
async function select(index){nodes.range.value=String(index);nodes.range.listeners.change();await settle();}
async function view(value){nodes.views.value=value;nodes.views.listeners.change();await settle();}
// Tiles are generated: find one by metric, and its parts by class.
const tile=metric=>nodes.tiles.children.find(t=>t.dataset?.metric===metric);
function part(element,name) {
  for(const child of element.children||[]){
    if(typeof child!=='object')continue;
    if(child.className.split(' ').includes(name))return child;
    const found=part(child,name);if(found)return found;
  }
}
async function main(){
  const appScript=[...html.matchAll(/<script\b([^>]*)>/g)].find(([,attributes])=>/\bsrc=["']app\.js["']/.test(attributes));
  assert(appScript&&/\bdefer\b/.test(appScript[1])&&!/\btype=/.test(appScript[1]),'classic deferred application script');
  const start=()=>vm.runInContext('(()=>{'+source+'\n})()',context);
  start();await settle();
  assert(nodes.status.textContent.startsWith('uptime: '),'initial data load: '+nodes.status.title);
  assert(strokes>30,'all seven graphs drawn');
  assert(hostRequests.every(name=>name.startsWith('h/fixture/')),'data comes from the connected host');
  assert(location.search==='?host=fixture','the device view is kept in the URL');
  const devices=nodes.views.children[0].children;
  assert(devices.length===2&&devices[1].textContent==='other-host (offline)','view picker lists hub hosts');
  assert(part(tile('cpu'),'graph-dots').children.length===3&&part(tile('pressure'),'graph-dots').children.length===3,'iowait and pressure series');
  assert(nodes.host.textContent&&document.title===nodes.host.textContent+' · quectop','host name identifies the device');
  const overall=part(tile('usage'),'value').textContent;
  assert(overall.endsWith('%'),'whole-machine summary');
  if(process.env.QUECTOP_TEST_EXPECT_OVERALL)assert(overall===process.env.QUECTOP_TEST_EXPECT_OVERALL,'weighted overall counter');
  const cores=Number(part(tile('usage'),'tile-sub').textContent.split(' ')[0]);
  assert(cores>0,'detected CPU count');
  assert(part(tile('cores'),'graph-dots').children.length===cores,'all cores shown in one graph');
  assert(part(tile('time'),'value').textContent.endsWith('%'),'user/system summary');
  const services=tile('services');
  if(process.env.QUECTOP_TEST_SERVICES){
    const mode=part(services,'chart-control'),table=part(services,'service-table'),rows=table.children[2],heading=table.children[1].children[0].children[1];
    assert(!services.hidden&&rows.children.length===1,'service table shown');
    mode.value='pss';mode.listeners.change();
    assert(heading.textContent==='PSS','PSS heading');
    assert(rows.children[0].children[1].textContent!=='—','PSS value');
    assert(rows.children[0].children[0].children[1].children[1].children.length===14,'memory breakdown fields');
    mode.value='rss';mode.listeners.change();
    assert(heading.textContent==='RSS','RSS selector');
    mode.value='memory';mode.listeners.change();
    assert(rows.children[0].children[1].title.includes('cgroup'),'cgroup semantics');
    assert(rows.children[0].children[2].title.includes('main service process'),'OpenRC uptime semantics');
  }else assert(services.hidden,'device view hides an empty service tile');
  assert(part(tile('temp'),'value').textContent.endsWith('°C'),'temperature summary');
  assert(requests.filter(x=>x==='1d.v1.bin').length===1,'initial selected history');
  await poll();assert(requests.filter(x=>x==='1d.v1.bin').length===1,'reuse completed windows');
  failNext=true;await select(4);
  assert(nodes.range.value==='1'&&nodes.status.dataset.state==='error','failed range change restores selection');
  for(const [index,file] of [[0,'1h.v1.bin'],[2,'7d.v1.bin'],[3,'30d.v1.bin'],[4,'90d.v1.bin']]){
    await select(index);assert(requests.includes(file),'range '+file);
    const match=nodes.detail.textContent.match(/· (\d+) windows/);
    assert(match&&Number(match[1])<=1000,'point budget');
  }
  const coreTile=tile('cores'),canvas=part(coreTile,'plot').children[0],tooltip=part(coreTile,'graph-tip'),details=part(coreTile,'sr');
  canvas.listeners.pointermove({clientX:392,clientY:100});await settle();assert(!tooltip.hidden,'floating tooltip');
  assert(details.textContent.includes('peak'),'peak timestamp accessible');
  assert(details.textContent.includes('maximum')&&!details.textContent.includes('range'),'CPU buckets keep no minimum');
  assert(!part(tile('ram'),'graph-guide').hidden,'cursor shared by every chart');
  part(tile('ram'),'plot').children[0].listeners.pointermove({clientX:392,clientY:100});await settle();
  assert(part(tile('ram'),'sr').textContent.includes('range'),'memory buckets keep a minimum');
  part(coreTile,'plot').listeners.pointerleave();await settle();assert(tooltip.hidden,'tooltip dismiss');
  canvas.listeners.focus();canvas.listeners.keydown({key:'Home',preventDefault:noop});await settle();const oldest=details.textContent;
  canvas.listeners.keydown({key:'End',preventDefault:noop});await settle();assert(oldest!==details.textContent,'keyboard history navigation');canvas.listeners.blur();
  nodes.theme.listeners.click();await settle();assert(document.documentElement.dataset.theme==='dark','theme switch');
  assert(nodes.status.dataset.state==='ok'&&nodes.status.textContent.endsWith(' · connected'),'connected device: '+nodes.status.textContent);
  hostsText='fixture\t0\tfixture-host\nother\t0\tother-host\n';await poll();
  assert(nodes.status.dataset.state==='error'&&nodes.status.textContent.endsWith(' · offline'),'hub reports the device offline: '+nodes.status.textContent);
  assert(nodes.views.children[0].children[0].textContent==='fixture-host (offline)','view picker follows hosts.txt');
  hostsText='fixture\t1\tfixture-host\nother\t0\tother-host\n';await poll();
  assert(nodes.status.dataset.state==='ok','device reconnects');
  const previous=part(tile('usage'),'value').textContent;failNext=true;await poll();assert(nodes.status.dataset.state==='error','failed refresh state');
  assert(part(tile('usage'),'value').textContent===previous,'retain last good measurements');
  await poll();
  // Boards: a new board opens in edit mode; added tiles and the name reach the URL and storage.
  await view('new');
  assert(nodes.host.textContent==='board1'&&!nodes['board-name'].hidden&&nodes.edit.attributes['aria-pressed']==='true','new board in edit mode');
  nodes['board-name'].value='Edge';
  for(const metric of ['usage','cpu','temp']){
    const form=nodes.tiles.children.at(-1),[device,chosen,add]=form.children;
    device.value='fixture';chosen.value=metric;add.listeners.click();await settle();
  }
  part(tile('temp'),'tile-remove').listeners.click();await settle();
  assert(location.search==='?board=board1&tiles=fixture~usage,fixture~cpu','board in the URL: '+location.search);
  const last=()=>nodes.tiles.children.at(-1);
  last().children[3].listeners.click();
  assert(last().children.length===2&&last().children[0].children[0]==='Add tile','Cancel closes the add form');
  last().children[0].listeners.click();
  assert(last().children.length===4,'Add tile reopens the form');
  assert(nodes['board-name'].value==='Edge','tile changes and form toggles preserve the draft name');
  await poll();
  assert(part(tile('usage'),'value').textContent.endsWith('%')&&part(tile('usage'),'tile-host').textContent==='fixture-host','board tile names its device');
  failStorage=true;nodes.edit.listeners.click();await poll();
  assert(!stored['quectop-boards']&&nodes.views.value==='b:-1'&&!nodes['board-name'].hidden,'failed save keeps the board unsaved and editable');
  assert(nodes.status.textContent.includes('Could not save')&&location.search.startsWith('?board=Edge&'),'failed save keeps a shareable draft and reports the failure');
  failStorage=false;nodes.edit.listeners.click();await settle();
  assert(JSON.parse(stored['quectop-boards'])[0].name==='Edge'&&location.search.startsWith('?board=Edge&'),'saved board');
  assert(nodes.views.children[1].children[0].textContent==='Edge'&&nodes.views.value==='b:0','board listed in the picker');
  failHosts=true;await poll();
  assert(nodes.status.textContent.includes('showing last snapshot')&&nodes.status.dataset.state==='error','board status follows a failed index refresh');
  failHosts=false;await poll();
  const now=Date.now;
  try{Date.now=()=>now()+120000;await poll();assert(nodes.status.textContent.includes('stale'),'board flags an unchanged snapshot');}
  finally{Date.now=now;}
  await poll();
  failNext=true;await poll();
  assert(nodes.status.textContent.includes('refresh failed')&&nodes.status.textContent.includes('showing last data'),'board status follows a failed device refresh');
  await poll();
  const historyRequests=requests.filter(x=>x==='90d.v1.bin').length;
  await view('h:other');assert(location.search==='?host=other','device choice is kept in the URL');
  await view('b:0');
  assert(requests.filter(x=>x==='90d.v1.bin').length===historyRequests+1,'leaving a host releases its cached history');
  wrongLength='1d.v1.bin';await select(1);
  assert(nodes.range.value==='4'&&nodes.status.title.includes('unexpected length'),'overlong streamed history rejected');
  wrongLength='';await poll();
  document.hidden=true;const count=requests.length;document.listeners.visibilitychange();await settle();assert(requests.length===count,'hidden tab skips fetch');
  const saved=JSON.parse(stored['quectop-boards'])[0];
  stored['quectop-boards']=JSON.stringify([saved,{name:'Recovered',tiles:[null,['fixture','constructor'],['..','temp'],['fixture','temp',1,2]]}]);
  location.search='?board=Edge&tiles=fixture~network';document.hidden=false;start();await settle();
  assert(nodes.views.value==='b:-1'&&nodes['delete'].hidden,'shared URL has no ownership of saved board');
  nodes.edit.listeners.click();nodes.edit.listeners.click();await settle();
  const subsequent=JSON.parse(stored['quectop-boards']);
  assert(JSON.stringify(subsequent[0])===JSON.stringify(saved)&&subsequent[2]?.name==='Edge (2)','shared board saves as a new board');
  nodes.edit.listeners.click();failStorage=true;nodes['delete'].listeners.click();await poll();
  assert(JSON.parse(stored['quectop-boards']).length===3&&nodes.views.value==='b:2'&&!nodes['delete'].hidden,'failed deletion leaves the saved board selected');
  assert(nodes.status.textContent.includes('Could not delete'),'failed deletion reports the failure');
  failStorage=false;nodes['delete'].listeners.click();await settle();
  assert(JSON.parse(stored['quectop-boards']).length===2,'delete retry removes the saved board');
  await view('b:1');
  assert(nodes.tiles.children.length===1&&tile('temp'),'valid entries survive malformed saved tiles');
  console.log('Application: seven graphs, five ranges and dynamic cores, cache, view picker, boards, pointer/keyboard details, theme, refresh errors and hidden tabs passed ('+assets+')');
}
main().catch(error=>{console.error(error);process.exitCode=1;});
