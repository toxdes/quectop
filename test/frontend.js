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
let strokes=0,failNext=false,timer;
const frames=[],noop=()=>{},context2d=new Proxy({},{get:(_,key)=>key==='stroke'?()=>strokes++:key==='measureText'?value=>({width:String(value).length*8.4}):noop,set:()=>true});
const node=()=>({value:'0',dataset:{},style:{},textContent:'',listeners:{},attributes:{},offsetWidth:210,offsetHeight:180,
  addEventListener(name,fn){this.listeners[name]=fn;},setAttribute(name,value){this.attributes[name]=value;},
  replaceChildren(...children){this.children=children;},append(...children){this.children=(this.children||[]).concat(children);},
  getBoundingClientRect:()=>({width:400,height:220,left:0,top:0}),getContext:()=>context2d,
  parentElement:{listeners:{},addEventListener(name,fn){this.listeners[name]=fn;}}});
const nodes=Object.fromEntries(ids.map(id=>[id,node()])),requests=[],hostRequests=[],windowListeners={};
const location={hash:'',reloads:0,reload(){this.reloads++;}};
let hostsText='fixture\t1\tfixture-host\nother\t0\tother-host\n';
nodes.range.value='1';
nodes['service-memory-mode'].value='pss';
const document={hidden:false,documentElement:node(),listeners:{},createElement:node,getElementById(id){if(!nodes[id])throw Error('Missing element '+id);return nodes[id];},querySelector:node,addEventListener(name,fn){this.listeners[name]=fn;}};
const context=vm.createContext({document,console,location,addEventListener(name,fn){windowListeners[name]=fn;},DataView,Uint8Array,Date,Math,Number,Error,Promise,AbortSignal,TextDecoder,
  devicePixelRatio:1,getComputedStyle:()=>({getPropertyValue:()=> '#567890'}),
  requestAnimationFrame(fn){frames.push(fn);return 1;},clearTimeout(){timer=null;},setTimeout(fn){timer=fn;return 1;},
  ResizeObserver:class{observe(){}},matchMedia:()=>({matches:false}),localStorage:{getItem:()=>null,setItem:noop},
  async fetch(name){
    if(name==='hosts.txt')return {ok:true,text:async()=>hostsText};
    hostRequests.push(name);name=name.replace(/^h\/fixture\//,'');requests.push(name);
    if(failNext){failNext=false;throw Error('simulated disconnect');}
    const bytes=fs.readFileSync(fixture+'/'+name);return {ok:true,arrayBuffer:async()=>bytes.buffer.slice(bytes.byteOffset,bytes.byteOffset+bytes.byteLength)};}
});
const assert=(value,message)=>{if(!value)throw Error(message);};
async function settle(){await Bun.sleep(5);while(frames.length)frames.shift()();}
async function poll(){const fn=timer;assert(fn,'refresh scheduled');await fn();await settle();}
async function select(index){nodes.range.value=String(index);nodes.range.listeners.change();await settle();}
async function main(){
  const appScript=[...html.matchAll(/<script\b([^>]*)>/g)].find(([,attributes])=>/\bsrc=["']app\.js["']/.test(attributes));
  assert(appScript&&/\bdefer\b/.test(appScript[1])&&!/\btype=/.test(appScript[1]),'classic deferred application script');
  vm.runInContext(source,context);await settle();
  assert(nodes.status.textContent.startsWith('uptime: '),'initial data load: '+nodes.status.title);
  assert(strokes>30,'all seven graphs drawn');
  assert(hostRequests.every(name=>name.startsWith('h/fixture/')),'data comes from the connected host');
  assert(!nodes.hosts.hidden&&nodes.hosts.children.length===2&&nodes.hosts.children[1].textContent==='other-host (offline)','host picker lists hub hosts');
  assert(nodes['dots-cpu'].children.length===3&&nodes['dots-pressure'].children.length===3,'iowait and pressure series');
  assert(nodes.host.textContent&&document.title===nodes.host.textContent+' · quectop','host name identifies the device');
  assert(nodes.overall.textContent.endsWith('%'),'whole-machine summary');
  if(process.env.QUECTOP_TEST_EXPECT_OVERALL)assert(nodes.overall.textContent===process.env.QUECTOP_TEST_EXPECT_OVERALL,'weighted overall counter');
  const cores=Number(nodes['overall-sub'].textContent.split(' ')[0]);
  assert(cores>0,'detected CPU count');
  assert(nodes['dots-cores'].children.length===cores,'all cores shown in one graph');
  assert(nodes.cpu.textContent.endsWith('%'),'user/system summary');
  if(process.env.QUECTOP_TEST_SERVICES){
    assert(!nodes['services-card'].hidden&&nodes.services.children.length===1,'service table shown');
    nodes['service-memory-mode'].value='pss';nodes['service-memory-mode'].listeners.change();
    assert(nodes['service-memory-heading'].textContent==='PSS','PSS heading');
    assert(nodes.services.children[0].children[1].textContent!=='—','PSS value');
    assert(nodes.services.children[0].children[0].children[1].children[1].children.length===14,'memory breakdown fields');
    nodes['service-memory-mode'].value='rss';nodes['service-memory-mode'].listeners.change();
    assert(nodes['service-memory-heading'].textContent==='RSS','RSS selector');
    nodes['service-memory-mode'].value='memory';nodes['service-memory-mode'].listeners.change();
    assert(nodes.services.children[0].children[1].title.includes('cgroup'),'cgroup semantics');
    assert(nodes.services.children[0].children[2].title.includes('main service process'),'OpenRC uptime semantics');
  }
  assert(nodes.temp.textContent.endsWith('°C'),'temperature summary');
  assert(requests.filter(x=>x==='1d.v1.bin').length===1,'initial selected history');
  await poll();assert(requests.filter(x=>x==='1d.v1.bin').length===1,'reuse completed windows');
  failNext=true;await select(4);
  assert(nodes.range.value==='1'&&nodes.status.dataset.state==='error','failed range change restores selection');
  for(const [index,file] of [[0,'1h.v1.bin'],[2,'7d.v1.bin'],[3,'30d.v1.bin'],[4,'90d.v1.bin']]){
    await select(index);assert(requests.includes(file),'range '+file);
    const match=nodes.detail.textContent.match(/· (\d+) windows/);
    assert(match&&Number(match[1])<=1000,'point budget');
  }
  const canvas=nodes['chart-cores'],tooltip=nodes['tip-cores'],details=nodes['detail-cores'];
  canvas.listeners.pointermove({clientX:392,clientY:100});await settle();assert(!tooltip.hidden,'floating tooltip');
  assert(details.textContent.includes('peak'),'peak timestamp accessible');
  assert(details.textContent.includes('maximum')&&!details.textContent.includes('range'),'CPU buckets keep no minimum');
  nodes['chart-memory'].listeners.pointermove({clientX:392,clientY:100});await settle();
  assert(nodes['detail-memory'].textContent.includes('range'),'memory buckets keep a minimum');
  canvas.parentElement.listeners.pointerleave();await settle();assert(tooltip.hidden,'tooltip dismiss');
  canvas.listeners.focus();canvas.listeners.keydown({key:'Home',preventDefault:noop});await settle();const oldest=details.textContent;
  canvas.listeners.keydown({key:'End',preventDefault:noop});await settle();assert(oldest!==details.textContent,'keyboard history navigation');canvas.listeners.blur();
  nodes.theme.listeners.click();await settle();assert(document.documentElement.dataset.theme==='dark','theme switch');
  assert(nodes.status.dataset.state==='ok'&&nodes.status.textContent.endsWith(' · connected'),'connected device: '+nodes.status.textContent);
  hostsText='fixture\t0\tfixture-host\nother\t0\tother-host\n';await poll();
  assert(nodes.status.dataset.state==='error'&&nodes.status.textContent.endsWith(' · offline'),'hub reports the device offline: '+nodes.status.textContent);
  assert(nodes.hosts.children[0].textContent==='fixture-host (offline)','host picker follows hosts.txt');
  hostsText='fixture\t1\tfixture-host\nother\t0\tother-host\n';await poll();
  assert(nodes.status.dataset.state==='ok','device reconnects');
  const previous=nodes.overall.textContent;failNext=true;await poll();assert(nodes.status.dataset.state==='error','failed refresh state');
  assert(nodes.overall.textContent===previous,'retain last good measurements');
  nodes.hosts.value='other';nodes.hosts.listeners.change();assert(location.hash==='other','host choice is kept in the URL');
  windowListeners.hashchange();assert(location.reloads===1,'switching hosts reloads the dashboard');
  document.hidden=true;const count=requests.length;document.listeners.visibilitychange();await settle();assert(requests.length===count,'hidden tab skips fetch');
  console.log('Application: seven graphs, five ranges and dynamic cores, cache, host picker, pointer/keyboard details, theme, refresh errors and hidden tabs passed ('+assets+')');
}
main().catch(error=>{console.error(error);process.exitCode=1;});
