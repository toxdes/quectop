'use strict';
// Exercise the shipped app as a black box, including minified release assets.
const fs=require('fs'),vm=require('vm');
const fixture=process.argv.at(-1),assets=process.env.QUECTOP_TEST_ASSETS||'web';
if(!fixture||fixture.endsWith('.js'))throw Error('Usage: bun test/frontend.js FIXTURE_DIRECTORY');
const html=fs.readFileSync(assets+'/index.html','utf8'),source=fs.readFileSync(assets+'/app.js','utf8');
const ids=[...html.matchAll(/id="([^"]+)"/g)].map(m=>m[1]);
let strokes=0,failNext=false,timer;
const frames=[],noop=()=>{},context2d=new Proxy({},{get:(_,key)=>key==='stroke'?()=>strokes++:key==='measureText'?value=>({width:String(value).length*8.4}):noop,set:()=>true});
const node=()=>({value:'0',dataset:{},style:{},textContent:'',listeners:{},attributes:{},offsetWidth:210,offsetHeight:180,
  addEventListener(name,fn){this.listeners[name]=fn;},setAttribute(name,value){this.attributes[name]=value;},
  replaceChildren(...children){this.children=children;},append(...children){this.children=(this.children||[]).concat(children);},
  getBoundingClientRect:()=>({width:400,height:220,left:0,top:0}),getContext:()=>context2d,
  parentElement:{listeners:{},addEventListener(name,fn){this.listeners[name]=fn;}}});
const nodes=Object.fromEntries(ids.map(id=>[id,node()])),requests=[];
nodes.range.value='1';
const document={hidden:false,documentElement:node(),listeners:{},createElement:node,getElementById(id){if(!nodes[id])throw Error('Missing element '+id);return nodes[id];},querySelector:node,addEventListener(name,fn){this.listeners[name]=fn;}};
const context=vm.createContext({document,console,DataView,Uint8Array,Date,Math,Number,Error,Promise,AbortSignal,TextDecoder,
  devicePixelRatio:1,getComputedStyle:()=>({getPropertyValue:()=> '#567890'}),
  requestAnimationFrame(fn){frames.push(fn);return 1;},clearTimeout(){timer=null;},setTimeout(fn){timer=fn;return 1;},
  ResizeObserver:class{observe(){}},matchMedia:()=>({matches:false}),localStorage:{getItem:()=>null,setItem:noop},
  async fetch(name){requests.push(name);if(failNext){failNext=false;throw Error('simulated disconnect');}const bytes=fs.readFileSync(fixture+'/'+name);return {ok:true,arrayBuffer:async()=>bytes.buffer.slice(bytes.byteOffset,bytes.byteOffset+bytes.byteLength)};}
});
const assert=(value,message)=>{if(!value)throw Error(message);};
async function settle(){await Bun.sleep(5);while(frames.length)frames.shift()();}
async function poll(){const fn=timer;assert(fn,'refresh scheduled');await fn();await settle();}
async function select(index){nodes.range.value=String(index);nodes.range.listeners.change();await settle();}
async function main(){
  vm.runInContext(source,context);await settle();
  assert(nodes.status.textContent.startsWith('uptime: '),'initial data load: '+nodes.status.title);
  assert(strokes>30,'all six graphs drawn');
  assert(nodes.overall.textContent.endsWith('%'),'whole-machine summary');
  if(process.env.QUECTOP_TEST_EXPECT_OVERALL)assert(nodes.overall.textContent===process.env.QUECTOP_TEST_EXPECT_OVERALL,'weighted overall counter');
  const cores=Number(nodes['overall-sub'].textContent.split(' ')[0]);
  assert(cores>0,'detected CPU count');
  assert(nodes['dots-cores'].children.length===cores,'all cores shown in one graph');
  assert(nodes.cpu.textContent.endsWith('%'),'user/system summary');
  assert(nodes.temp.textContent.endsWith('°C'),'temperature summary');
  assert(requests.filter(x=>x==='1d.v7.bin').length===1,'initial selected history');
  await poll();assert(requests.filter(x=>x==='1d.v7.bin').length===1,'reuse completed windows');
  failNext=true;await select(4);
  assert(nodes.range.value==='1'&&nodes.status.dataset.state==='error','failed range change restores selection');
  for(const [index,file] of [[0,'1h.v7.bin'],[2,'7d.v7.bin'],[3,'30d.v7.bin'],[4,'90d.v7.bin']]){
    await select(index);assert(requests.includes(file),'range '+file);
    const match=nodes.detail.textContent.match(/· (\d+) windows/);
    assert(match&&Number(match[1])<=1000,'point budget');
  }
  const canvas=nodes['chart-cores'],tooltip=nodes['tip-cores'],details=nodes['detail-cores'];
  canvas.listeners.pointermove({clientX:392,clientY:100});await settle();assert(!tooltip.hidden,'floating tooltip');
  assert(details.textContent.includes('peak'),'peak timestamp accessible');
  canvas.parentElement.listeners.pointerleave();await settle();assert(tooltip.hidden,'tooltip dismiss');
  canvas.listeners.focus();canvas.listeners.keydown({key:'Home',preventDefault:noop});await settle();const oldest=details.textContent;
  canvas.listeners.keydown({key:'End',preventDefault:noop});await settle();assert(oldest!==details.textContent,'keyboard history navigation');canvas.listeners.blur();
  nodes.theme.listeners.click();await settle();assert(document.documentElement.dataset.theme==='dark','theme switch');
  const previous=nodes.overall.textContent;failNext=true;await poll();assert(nodes.status.dataset.state==='error','failed refresh state');
  assert(nodes.overall.textContent===previous,'retain last good measurements');
  document.hidden=true;const count=requests.length;document.listeners.visibilitychange();await settle();assert(requests.length===count,'hidden tab skips fetch');
  console.log('Application: six graphs, five ranges and dynamic cores, cache, pointer/keyboard details, theme, refresh errors and hidden tabs passed ('+assets+')');
}
main().catch(error=>{console.error(error);process.exitCode=1;});
