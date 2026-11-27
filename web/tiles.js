/* Tiles: one metric of one device. The device view and every board render
 * through the same registry, so a metric is defined once. */
import {spans} from './codec.js';
import {percent,age,byteScale,byteUnits,capacity,shortSpeed,speed,capacityPair,serviceAge,seriesColor} from './format.js';

export const view={index:0,colors:null};
const h=(tag,className,...children)=>{const e=document.createElement(tag);if(className)e.className=className;e.append(...children);return e;};
let serial=0;
const unique=part=>part+'-'+(++serial);

const temperatureText=n=>n==null?'—':n.toFixed(1)+' °C';
function summary(t) {
  const d=t.def,s=t.src.state;
  if(!s)return;
  t.qualifier.textContent=d.qualifier(t,s);t.value.textContent=d.value(t,s);t.sub.textContent=d.sub(t,s);
  if(d.toggle){t.body.disabled=d.disabled?.(t,s)||false;t.body.setAttribute('aria-label',d.label+' '+t.qualifier.textContent+(d.next?'. '+d.next(t,s):''));}
}
const cpu=(s,f)=>s.latest[s.format[f]];
const iface=t=>t.src.iface;
/* Summary tiles: label, teal qualifier, value, muted secondary line. */
const summaries={
  usage:{label:'CPU usage',qualifier:()=>'total',value:(t,s)=>percent(cpu(s,'overall')),sub:(t,s)=>s.cores+' logical '+(s.cores===1?'CPU':'CPUs')},
  time:{label:'CPU time',toggle:t=>{t.system=!t.system;},qualifier:t=>t.system?'system':'user',
    value:(t,s)=>percent(cpu(s,t.system?'system':'user')),sub:(t,s)=>(t.system?'user ':'system ')+percent(cpu(s,t.system?'user':'system')),
    next:t=>'Show '+(t.system?'user':'system')+' time'},
  memory:{label:'Memory',toggle:t=>{t.free=!t.free;},qualifier:t=>t.free?'free':'used',
    value:(t,s)=>{const p=memoryPercent(t,s);return p==null?'—':capacity(s.memory*1024*p/100);},
    sub:(t,s)=>percent(memoryPercent(t,s))+' of '+capacity(s.memory*1024),next:t=>'Show '+(t.free?'used':'free')+' memory',
    title:'Free means MemAvailable, including reclaimable cache. Capacities are estimated from the stored 0.1% reading; K/M/G are binary units (K = 1024 bytes).'},
  swap:{label:'Swap',qualifier:()=>'used',value:(t,s)=>s.swap?cpu(s,'swap')==null?'—':capacity(s.swap*1024*cpu(s,'swap')/100):'off',
    sub:(t,s)=>s.swap?percent(cpu(s,'swap'))+' of '+capacity(s.swap*1024):'no swap configured'},
  temp:{label:'Temperature',qualifier:()=>'CPU',value:(t,s)=>temperatureText(cpu(s,'temp')),sub:(t,s)=>cpu(s,'temp')==null?'sensor unavailable':'thermal sensor'},
  network:{label:'Network',toggle:t=>{t.src.iface=(iface(t)+1)%t.src.state.interfaces;},qualifier:(t,s)=>s.interfaceNames[iface(t)]||'none',
    value:(t,s)=>{const r=s.latest,f=s.format.network+2*iface(t);return s.interfaces?'↑'+shortSpeed(r[f+1])+' ↓'+shortSpeed(r[f]):'—';},
    sub:()=>'per second',disabled:(t,s)=>s.interfaces<2,next:()=>'Show next interface',
    title:'↑ upload, ↓ download. Rates per second in decimal units (K = 1000 bytes).'}
};
function memoryPercent(t,s){const p=cpu(s,'memory');return p==null?null:t.free?100-p:p;}

/* Chart tiles. fields/labels read the device's layout; the tile holds view toggles. */
const charts={
  cores:{title:'CPU per core',unit:'%',fields:(t,s)=>s.cpuIds.map((_,i)=>i),labels:(t,s)=>s.cpuIds.map(id=>'CPU '+id)},
  cpu:{title:'CPU usage',unit:'%',fields:(t,s)=>[s.format.user,s.format.system,s.format.iowait],labels:()=>['user','system','iowait'],legend:true},
  pressure:{title:'Pressure',unit:'%',fields:(t,s)=>[0,1,2].map(i=>s.format.pressure+i),labels:()=>['CPU','memory','I/O'],legend:true,
    hint:'Share of time at least one task was stalled waiting for this resource (Linux PSI)'},
  ram:{title:t=>t.swap?'Swap':'Memory',unit:'%',fields:(t,s)=>[t.swap?s.format.swap:s.format.memory],labels:t=>[t.swap?'swap used':'RAM used'],legend:true},
  frequency:{title:'CPU frequency',unit:'GHz',divisor:1000,fields:(t,s)=>s.frequencyIds.map((_,i)=>s.format.frequency+i),labels:(t,s)=>s.frequencyIds.map(id=>'CPU '+id+' policy'),
    note:(t,s)=>s.frequencies?(s.frequencyHardware.every(Boolean)?'hardware-reported':'driver-reported'):'unavailable',
    hint:'One reading per shared-clock policy; labels identify its leading CPU. Driver-reported clocks may differ from instantaneous hardware clocks.'},
  temperature:{title:'Temperature',unit:'°C',fields:(t,s)=>[s.format.temp],labels:()=>['CPU'],note:(t,s)=>cpu(s,'temp')==null?'unavailable':'CPU'},
  traffic:{title:'Network',unit:'B/s',fields:(t,s)=>s.interfaces?[s.format.network+2*iface(t)+1,s.format.network+2*iface(t)]:[],labels:()=>['upload','download'],legend:true,
    note:(t,s)=>s.interfaceNames[iface(t)]||'no interfaces'}
};
const panels={disks:{title:'Disks'},services:{title:'Services'}};
/* Order of the device view; boards pick from the same keys. */
export const metricKeys=[...Object.keys(summaries),...Object.keys(charts),...Object.keys(panels)];
export const metricLabel=key=>summaries[key]?.label||(typeof charts[key]?.title==='string'?charts[key].title+' chart':key==='ram'?'Memory chart':panels[key].title);
export const isChart=key=>key in charts;

export function formatValue(t,n) {
  const m=t.metric;
  return n==null?'—':m==='traffic'?speed(n):m==='frequency'?Number((n/1000).toFixed(3))+' GHz':m==='temperature'?temperatureText(n):n.toFixed(1)+'%';
}

/* Builds the element for one tile. host is the label shown on boards. */
export function createTile(metric,src,host) {
  const t={metric,src,key:unique('tile'),el:h('article','tile '+(summaries[metric]?'summary':'chart'))};
  t.el.dataset.metric=metric;
  if(host)t.el.append(t.host=h('p','tile-host',host));
  if(summaries[metric])buildSummary(t);
  else if(charts[metric])buildChart(t);
  else if(metric==='disks')buildDisks(t);
  else buildServices(t);
  return t;
}
function buildSummary(t) {
  const d=t.def=summaries[t.metric];
  t.qualifier=h('span');t.value=h('strong','value','—');t.sub=h('span','tile-sub');
  const heading=h(d.toggle?'span':'h2','tile-heading',d.label+' ',t.qualifier);
  t.body=h(d.toggle?'button':'div','summary-body',heading,t.value,t.sub);
  if(d.title)t.body.title=d.title;
  if(d.toggle)t.body.addEventListener('click',()=>{if(t.src.state&&!t.body.disabled){d.toggle(t);t.src.touch();}});
  t.el.append(t.body);t.update=()=>summary(t);
}
function buildChart(t) {
  const d=t.def=charts[t.metric];
  t.title=h('span');t.unit=h('span','unit');t.legend=h('p','legend-head');
  if(d.hint)t.legend.title=d.hint;
  const head=h('div','chart-head',h('h2','',t.title,' ',t.unit),t.legend);
  t.canvas=h('canvas');t.guide=h('div','graph-guide');t.dots=h('div','graph-dots');t.tip=h('div','graph-tip');t.detail=h('output','sr');
  t.detail.id=unique('detail');t.guide.hidden=t.tip.hidden=true;
  for(const e of[t.guide,t.dots,t.tip])e.setAttribute('aria-hidden','true');
  t.canvas.tabIndex=0;t.canvas.setAttribute('role','img');t.canvas.setAttribute('aria-describedby','chart-help '+t.detail.id);
  t.plot=h('div','plot',t.canvas,t.guide,t.dots,t.tip);
  t.el.append(head,t.plot);
  if(t.metric==='cores'){
    t.mode=h('select','chart-control');t.mode.setAttribute('aria-label','Per-core CPU view');
    for(const v of['Graph','Bars'])t.mode.append(new Option(v,v.toLowerCase()));
    t.mode.addEventListener('change',()=>{clearInspection();t.plot.hidden=t.coreLegend.hidden=t.mode.value==='bars';t.bars.hidden=!t.plot.hidden;t.src.touch();});
    head.append(t.mode);
    t.coreLegend=h('p','legend');t.bars=h('ul','core-bars');t.bars.hidden=true;t.bars.tabIndex=0;t.bars.setAttribute('aria-label','Latest CPU usage per core');
    t.el.append(t.coreLegend,t.bars);
  }
  if(t.metric==='ram'){
    t.toggle=h('button','chart-control');
    t.toggle.addEventListener('click',()=>{t.swap=!t.swap;clearInspection();t.src.touch();});
    head.append(t.toggle);
  }
  t.el.append(t.detail);
  bindInspection(t);
  t.update=()=>updateChart(t);
}
function updateChart(t) {
  const d=t.def,s=t.src.state;
  if(!s)return;
  t.fields=d.fields(t,s);t.labels=d.labels(t,s);
  t.title.textContent=typeof d.title==='function'?d.title(t):d.title;
  if(t.metric!=='traffic')t.unit.textContent='('+d.unit+')';
  const signature=t.fields.join(',')+'|'+t.labels.join(',')+'|'+(d.note?d.note(t,s):'');
  if(t.signature!==signature){
    t.signature=signature;
    const dots=t.labels.map((label,k)=>h('span','',dot(k),label));
    if(d.legend)t.legend.replaceChildren(...(t.metric==='traffic'?[h('span','',d.note(t,s)+' ·'),' ']:[]),...dots.flatMap(e=>[e,' ']));
    else if(d.note)t.legend.textContent=d.note(t,s);
    if(t.metric==='cores'){t.coreLegend.replaceChildren(...s.cpuIds.map((id,k)=>h('span','',dot(k),id)).flatMap(e=>[e,' ']));t.meters=null;}
    t.markers=t.fields.map((_,k)=>{const marker=dot(k);marker.className='graph-dot';marker.hidden=true;return marker;});
    t.dots.replaceChildren(...t.markers);t.tipKey='';
  }
  if(t.toggle){t.toggle.textContent=t.swap?'show RAM':'show swap';t.toggle.setAttribute('aria-pressed',String(Boolean(t.swap)));t.toggle.setAttribute('aria-label',t.swap?'Show RAM history':'Show swap history');}
  if(t.metric==='cores'&&!t.bars.hidden)renderBars(t,s);
}
function dot(k){const marker=h('i');marker.style.background=seriesColor(k);return marker;}
function renderBars(t,s) {
  if(!t.meters){
    t.meters=s.cpuIds.map((id,k)=>{
      const label=h('span','core-id',id),bar=h('meter','core-meter'),value=h('span','core-value');
      label.title='CPU '+id;bar.min=0;bar.max=100;bar.style.color=seriesColor(k);bar.setAttribute('aria-label','CPU '+id+' usage');
      return {item:h('li','',label,bar,value),bar,value};
    });
    t.bars.style.setProperty('--core-rows',String(Math.ceil(s.cores/2)));
    t.bars.dataset.columns=String(s.cores>1?2:1);
    t.bars.replaceChildren(...t.meters.map(m=>m.item));
  }
  t.meters.forEach((m,k)=>{
    const current=s.latest[k];
    m.bar.value=current??0;m.bar.dataset.available=String(current!=null);
    m.bar.setAttribute('aria-valuetext',current==null?'unavailable':percent(current));m.value.textContent=percent(current);
  });
}

const field=(row,part,t,k)=>row[part][t.fields[k]];
/* Draws one chart from its device's visible windows. */
export function draw(t) {
  const s=t.src.state,visible=t.src.visible,colors=view.colors;
  if(!s||!t.fields||t.plot.hidden)return;
  const canvas=t.canvas,rect=canvas.getBoundingClientRect(),w=Math.round(rect.width),hgt=Math.round(rect.height),dpr=Math.min(devicePixelRatio||1,2);
  if(!w||!hgt)return;
  const pw=Math.round(w*dpr),ph=Math.round(hgt*dpr),c=canvas.getContext('2d');
  if(canvas.width!==pw||canvas.height!==ph){canvas.width=pw;canvas.height=ph;}
  c.setTransform(dpr,0,0,dpr,0,0);c.clearRect(0,0,w,hgt);
  const index=view.index,left=60,right=w-8,top=8,bottom=hgt-32,width=s.widths[index]*1000;
  // A short history fills the plot instead of sitting at its right edge.
  const end=s.latestTime,start=Math.max(end-spans[index]*1000,Math.min(visible[0]?.start??end,end-12*width));
  let divisor=t.def.divisor||1,unit='',largest=0;
  if(t.metric==='traffic') {
    for(const r of visible)for(const f of t.fields)largest=Math.max(largest,r.max[f]||0);
    let u=0;while(largest/1000**u>=999.95&&u<byteUnits.length-1)u++;divisor=1000**u;unit=byteUnits[u];
    t.unit.textContent='('+unit+'/s)';
  }
  const isPercent=t.def.unit==='%';
  let max=-Infinity,min=Infinity,hasValue=false;
  for(const r of visible)for(let k=0;k<t.fields.length;k++) {
    const lo=field(r,'min',t,k)??field(r,'mean',t,k),hi=field(r,'max',t,k);
    if(hi!=null){hasValue=true;max=Math.max(max,hi/divisor);min=Math.min(min,lo/divisor);}
  }
  if(hasValue) {
    const padding=Math.max((max-min)*.1,t.metric==='traffic'?Math.max(.01/divisor,10**Math.floor(Math.log10(max||1))*.01):t.metric==='frequency'?.05:.5);
    min-=padding;max+=padding;
    if(t.metric!=='temperature')min=Math.max(0,min);
    if(isPercent)max=Math.min(100,max);
  }else{min=0;max=isPercent?100:1;}
  // A 1, 2 or 5 step for about five intervals; the bounds snap to it.
  const raw=(max-min)/5,power=10**Math.floor(Math.log10(raw)),fraction=raw/power;
  const step=(fraction<=1?1:fraction<=2?2:fraction<=5?5:10)*power;
  min=Math.floor(min/step+1e-9)*step;max=Math.ceil(max/step-1e-9)*step;
  const x=v=>left+(v-start)/(end-start)*(right-left),y=v=>bottom-(v-min)/(max-min)*(bottom-top);
  let axisDigits=0;while(axisDigits<9&&Math.abs(Math.round(step*10**axisDigits)-step*10**axisDigits)>1e-6)axisDigits++;
  const center=r=>Math.max(start,Math.min(end,r.start+(r.current?Math.max(0,end-r.start)/2:width/2)));
  c.font='14px '+colors.font;c.lineWidth=.6;c.textBaseline='middle';
  for(let i=0,lines=Math.round((max-min)/step);i<=lines;i++) {
    const value=min+step*i,py=y(value);
    c.strokeStyle=colors.grid;c.beginPath();c.moveTo(left,py);c.lineTo(right,py);c.stroke();
    c.fillStyle=colors.muted;c.textAlign='right';
    c.fillText(t.metric!=='traffic'?String(Number(value.toFixed(axisDigits))):value?(([n,u])=>Number(n.toFixed(2))+u)(byteScale(value*divisor,1000)):'0',left-6,py);
  }
  // Edge labels shift inward by half their width, so intervals leave room for 1.5 labels.
  const timeLabel=v=>{const date=new Date(v);return end-start>2*86400000?date.toLocaleDateString(undefined,{month:'short',day:'numeric'}):date.toLocaleTimeString(undefined,{hour:'2-digit',minute:'2-digit',second:end-start<=600000?'2-digit':undefined,hour12:false});};
  const ticks=Math.max(2,Math.min(w<380?3:5,Math.floor((right-left)/(c.measureText(timeLabel(end)).width*1.5+16))+1));c.textAlign='center';
  for(let i=0;i<ticks;i++) {
    const v=start+(end-start)*i/(ticks-1),px=x(v);
    c.strokeStyle=colors.grid;c.beginPath();c.moveTo(px,top);c.lineTo(px,bottom);c.stroke();
    c.fillStyle=colors.muted;
    const label=timeLabel(v),half=c.measureText(label).width/2;
    c.fillText(label,Math.max(left+half,Math.min(right-half,px)),hgt-12);
  }
  c.save();c.beginPath();c.rect(left,top,right-left,bottom-top);c.clip();
  for(let k=0;k<t.fields.length;k++) {
    c.strokeStyle=c.fillStyle=colors.palette[k];
    let segment=[];
    const paint=()=>{
      if(!segment.length)return;
      c.globalAlpha=.12;c.beginPath();
      segment.forEach((r,i)=>{const px=x(center(r)),py=y(field(r,'max',t,k)/divisor);if(i)c.lineTo(px,py);else c.moveTo(px,py);});
      for(let i=segment.length-1;i>=0;i--){const r=segment[i];c.lineTo(x(center(r)),y((field(r,'min',t,k)??field(r,'mean',t,k))/divisor));}
      c.closePath();c.fill();
      // Exact peak times locate brief excursions within each averaging window.
      c.globalAlpha=.5;c.lineWidth=.8;c.beginPath();
      for(const r of segment) {
        const px=x(Math.max(start,Math.min(end,r.peak[t.fields[k]]))),mean=field(r,'mean',t,k)/divisor;
        c.moveTo(px,y(mean));c.lineTo(px,y(field(r,'max',t,k)/divisor));
      }
      c.stroke();c.globalAlpha=1;c.lineWidth=1.2;c.beginPath();
      segment.forEach((r,i)=>{const px=x(center(r)),py=y(field(r,'mean',t,k)/divisor);if(i)c.lineTo(px,py);else c.moveTo(px,py);});c.stroke();
      if(segment.length===1){const r=segment[0];c.beginPath();c.arc(x(center(r)),y(field(r,'mean',t,k)/divisor),2,0,Math.PI*2);c.fill();}
      segment=[];
    };
    let previous=0;
    for(const r of visible){if(field(r,'mean',t,k)==null||previous&&r.start-previous>width*1.5)paint();if(field(r,'mean',t,k)!=null)segment.push(r);previous=r.start;}
    paint();
  }
  c.restore();
  if(!hasValue){c.fillStyle=colors.muted;c.textAlign='center';c.fillText(t.metric==='temperature'?'Sensor unavailable':t.metric==='pressure'&&t.fields.every(f=>s.latest[f]==null)?'Pressure stall information unavailable':t.metric==='frequency'?'CPU frequency unavailable':t.metric==='ram'&&t.swap&&!s.swap?'No swap configured':'Waiting for samples',(left+right)/2,(top+bottom)/2);}
  t.geometry={w,h:hgt,left,right,top,bottom,width,start,end,x,y,center,divisor};
  t.tipKey='';
  t.canvas.setAttribute('aria-label',t.title.textContent+' history: average lines and minimum/maximum bands. Latest: '+t.fields.map((f,k)=>t.labels[k]+' '+formatValue(t,s.latest[f])).join(', '));
}

/* One cursor time is shared by every chart, across devices; each chart shows
 * its own window nearest to it, and the chart under the pointer its tooltip. */
let inspection=null,inspectionFrame=0;
export let charted=[];
export const setCharted=list=>{charted=list;};
export function queueInspection(){if(!inspectionFrame)inspectionFrame=requestAnimationFrame(()=>{inspectionFrame=0;renderInspection();});}
export function clearInspection(){inspection=null;queueInspection();}
function nearest(t,time) {
  const visible=t.src.visible,g=t.geometry;
  if(!visible.length||!g)return null;
  let low=0,high=visible.length-1;
  while(low<high){const mid=(low+high)>>1;if(g.center(visible[mid])<time)low=mid+1;else high=mid;}
  if(low&&Math.abs(g.center(visible[low-1])-time)<Math.abs(g.center(visible[low])-time))low--;
  return Math.abs(g.center(visible[low])-time)>g.width*1.5?null:visible[low];
}
function detailText(t,row) {
  const fmt=n=>n==null?'unavailable':formatValue(t,n);
  return new Date(row.start).toLocaleString()+' · '+t.fields.map((f,k)=>{
    const min=field(row,'min',t,k),max=fmt(field(row,'max',t,k));
    return t.labels[k]+' average '+fmt(field(row,'mean',t,k))+(min==null?', maximum '+max:', range '+fmt(min)+'–'+max)+', peak '+new Date(row.peak[f]).toLocaleTimeString();
  }).join(' · ');
}
function renderInspection() {
  for(const t of charted) {
    const g=t.geometry,row=inspection&&!t.plot.hidden&&nearest(t,inspection.time);
    t.guide.hidden=!row;t.tip.hidden=true;
    if(!row){for(const marker of t.markers||[])marker.hidden=true;continue;}
    const px=g.x(g.center(row));
    t.guide.style.left=px+'px';t.guide.style.top=g.top+'px';t.guide.style.height=(g.bottom-g.top)+'px';
    t.fields.forEach((_,k)=>{
      const marker=t.markers[k],value=field(row,'mean',t,k);
      marker.hidden=value==null;
      if(value!=null){marker.style.left=px+'px';marker.style.top=Math.max(g.top,Math.min(g.bottom,g.y(value/g.divisor)))+'px';}
    });
    if(inspection.tile!==t)continue;
    const key=row.start+':'+t.src.state.generated+':'+t.fields.join(',');
    t.tip.hidden=false;
    if(t.tipKey!==key) {
      const time=h('time','',new Intl.DateTimeFormat(undefined,{year:'numeric',month:'short',day:'numeric',hour:'2-digit',minute:'2-digit',second:'2-digit'}).format(new Date(g.center(row))));
      const values=h('div','tip-values'+(t.fields.length>8?' tip-columns':''),...t.fields.map((_,k)=>h('div','tip-row',dot(k),h('span','',t.labels[k]),h('b','',formatValue(t,field(row,'mean',t,k))))));
      t.tip.replaceChildren(time,values);t.detail.textContent=detailText(t,row);
      t.tipKey=key;t.tipWidth=t.tip.offsetWidth;t.tipHeight=t.tip.offsetHeight;
    }
    const top=inspection.y??g.top+12;
    t.tip.style.left=Math.max(6,Math.min(g.w-t.tipWidth-6,px+12+t.tipWidth>g.w-6?px-t.tipWidth-12:px+12))+'px';
    t.tip.style.top=Math.max(6,Math.min(g.h-t.tipHeight-6,top-12))+'px';
  }
}
function bindInspection(t) {
  const canvas=t.canvas,inspect=(row,y,keyboard)=>{inspection={tile:t,time:t.geometry.center(row),y,keyboard};queueInspection();};
  function pointer(event) {
    const g=t.geometry;if(!g)return;
    const rect=canvas.getBoundingClientRect(),px=event.clientX-rect.left,py=event.clientY-rect.top;
    if(px<g.left||px>g.right||py<g.top||py>g.bottom){clearInspection();return;}
    const row=nearest(t,g.start+(px-g.left)/(g.right-g.left)*(g.end-g.start));
    if(row)inspect(row,py,false);else clearInspection();
  }
  canvas.addEventListener('pointermove',pointer);canvas.addEventListener('pointerdown',pointer);
  canvas.addEventListener('pointercancel',clearInspection);
  t.plot.addEventListener('pointerleave',()=>{if(document.activeElement!==canvas)clearInspection();});
  canvas.addEventListener('focus',()=>{const v=t.src.visible;if(v.length&&t.geometry&&inspection?.tile!==t)inspect(v[v.length-1],undefined,true);});
  canvas.addEventListener('blur',()=>{if(inspection?.tile===t)clearInspection();});
  canvas.addEventListener('keydown',event=>{
    if(event.key==='Escape'){event.preventDefault();clearInspection();return;}
    if(inspection?.tile===t&&['PageUp','PageDown'].includes(event.key)){event.preventDefault();t.tip.scrollTop+=(event.key==='PageDown'?1:-1)*t.tip.clientHeight*.8;return;}
    const v=t.src.visible;
    if(!v.length||!t.geometry||!['ArrowLeft','ArrowRight','Home','End'].includes(event.key))return;
    event.preventDefault();
    const current=inspection&&nearest(t,inspection.time);
    let cursor=current?v.indexOf(current):v.length-1;
    if(event.key==='Home')cursor=0;else if(event.key==='End')cursor=v.length-1;
    else cursor=Math.max(0,Math.min(v.length-1,cursor+(event.key==='ArrowRight'?1:-1)));
    inspect(v[cursor],undefined,true);
  });
}

function buildDisks(t) {
  t.count=h('p','','mounted');t.list=h('ul','disk-list',h('li','','Waiting for capacities…'));t.status=h('p','disk-status');
  t.list.tabIndex=0;t.list.setAttribute('aria-label','Mounted disk capacities');
  t.el.append(h('div','chart-head',h('h2','','Disks'),t.count),h('div','disk-body',t.list,t.status));
  t.update=()=>updateDisks(t);
}
function updateDisks(t) {
  const snapshot=t.src.disks,error=t.src.diskError;
  if(snapshot!==t.rendered||error!==t.renderedError) {
    t.rendered=snapshot;t.renderedError=error;
    if(!snapshot){t.list.textContent=error?'Capacity data unavailable':'Waiting for capacities…';t.count.textContent=error?'unavailable':'mounted';return;}
    t.count.textContent=snapshot.rows.length+' mounted'+(snapshot.partial?' · partial':'');
    t.list.replaceChildren(...snapshot.rows.map(row=>{
      const label=h('span','disk-label',row.source.replace(/^\/dev\//,'')+' · '+row.mount),bar=h(row.valid?'meter':'span','disk-meter'),values=h('span','disk-values'),available=h('span','disk-available');
      label.title=row.source+' · '+row.mount;
      if(row.valid){
        bar.min=0;bar.max=row.total;bar.value=row.used;
        bar.setAttribute('aria-label',row.source+' used space');bar.setAttribute('aria-valuetext',percent(100*row.used/row.total)+' used');
        values.textContent=capacityPair(row.used,row.total);values.setAttribute('aria-label',capacity(row.used)+' used of '+capacity(row.total));
        available.append(capacity(row.available),h('small','','avail'));
      }else{bar.setAttribute('aria-hidden','true');values.textContent='capacity unavailable';available.textContent='—';}
      return h('li','',label,bar,values,available);
    }));
    if(!snapshot.rows.length)t.list.textContent='No mounted local filesystems';
  }
  if(snapshot)t.status.textContent=error?'Refresh failed · showing previous capacities':'Updated '+age(Math.max(0,Date.now()-snapshot.generated))+' ago';
}

const serviceMemory={
  memory:{label:'Cgroup',description:'Memory charged to the service cgroup, including descendants'},
  rss:{label:'RSS',description:'Resident mapped memory; shared pages counted once per process'},
  pss:{label:'PSS',description:'Mapped memory with shared pages divided among sharing processes'},
  uss:{label:'Private',description:'Private clean and dirty mapped pages; excludes hugetlb',rounded:true},
  anon:{label:'PSS anonymous',rounded:true},
  file:{label:'PSS file',rounded:true,title:'Mapped libraries and other file-backed pages'},
  shmem:{label:'PSS shared memory',rounded:true}
};
const serviceStates=['unavailable','inactive','active','failed','activating','stopping','reloading','unavailable'];
function buildServices(t) {
  t.count=h('p');t.mode=h('select','chart-control');t.mode.setAttribute('aria-label','Service memory measurement');
  for(const key of['pss','rss','uss','memory'])t.mode.append(new Option(serviceMemory[key].label,key));
  t.mode.addEventListener('change',()=>updateServices(t));
  t.heading=h('th','','PSS');t.rows=h('tbody');
  const columns=[h('th','','Name'),t.heading,h('th','','Uptime')];
  for(const e of columns)e.scope='col';
  const table=h('table','service-table',h('caption','sr','Current system service memory and uptime; expand a service for its memory breakdown'),h('thead','',h('tr','',...columns)),t.rows);
  t.body=h('div','service-body',table);t.body.tabIndex=0;t.body.setAttribute('aria-label','Service memory and uptime');
  t.el.append(h('div','chart-head',h('h2','','Services'),t.count,t.mode),t.body);
  t.update=()=>updateServices(t);
}
function updateServices(t) {
  const rows=t.src.state?.services||[];
  t.el.hidden=t.optional&&!rows.length;
  const key=t.mode.value||'pss',selected=serviceMemory[key];
  t.heading.textContent=selected.label;
  const sorted=[...rows].sort((a,b)=>a.name.localeCompare(b.name)),signature=sorted.map(row=>row.name+(row.openrc?'\0o':'')).join('\n');
  if(signature!==t.signature) {
    t.signature=signature;t.count.textContent=rows.length?rows.length+' tracked':'none tracked';
    t.views=sorted.map(row=>{
      const name=h('th','',h('span','',row.openrc?row.name:row.name.replace(/\.service$/,''))),memory=h('td'),uptime=h('td'),list=h('dl'),values={};
      name.scope='row';name.title=row.name+(row.openrc?' (OpenRC)':'');
      for(const [field,definition] of Object.entries(serviceMemory)){
        const term=h('dt','',definition.label);if(definition.title)term.title=definition.title;
        list.append(term,values[field]=h('dd'));
      }
      name.append(h('details','',h('summary','','Breakdown'),list));
      return {item:h('tr','',name,memory,uptime),memory,uptime,values};
    });
    t.rows.replaceChildren(...t.views.map(v=>v.item));
  }
  sorted.forEach((row,i)=>{
    const v=t.views[i];
    for(const [field,definition] of Object.entries(serviceMemory)){
      v.values[field].textContent=capacity(row[field]);
      v.values[field].title=definition.rounded&&row[field]!=null?'Stored rounded to four significant digits':'';
    }
    v.memory.textContent=capacity(row[key]);v.memory.title=row[key]==null?'Whole-service '+selected.label+' unavailable':selected.description;
    v.uptime.textContent=row.uptime==null?serviceStates[row.status]:serviceAge(row.uptime);
    v.uptime.title=row.uptime==null?serviceStates[row.status]:row.processAge?'Age of the main service process':'Time since the service became active';
  });
}
