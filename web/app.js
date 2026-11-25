'use strict';
import {spans,files,liveFile,disksFile,decodeLive,decodeHistory,decodeDisks,skipped as damagedRecords} from './codec.js';
const $=id=>document.getElementById(id);
const range=$('range'),view=$('view'),statusLine=$('status'),hostSelect=$('hosts');
const colorNames=['--teal','--violet','--third','--fourth','--core5','--core6','--core7','--core8'];
const seriesColor=k=>k<colorNames.length?'var('+colorNames[k]+')':'hsl('+((k*137.508)%360).toFixed(1)+' 55% var(--series-lightness))';
let coreTopology='',cpuSystem=false,memoryFree=false,showSwap=false,interfaceIndex=0,selectedInterface;
let coreBarsTopology='',coreMeters=[];
let disksSnapshot,diskError='';
let state,visible=[],historyCache,pollTimer,busy=false,frame=0,inspection=null,inspectionFrame=0,hostId='';
const noProbes=Error('No devices have connected to this hub yet');
const inspectionDate=new Intl.DateTimeFormat(undefined,{year:"numeric",month:"short",day:"numeric",hour:"2-digit",minute:"2-digit",second:"2-digit"});
const percent=n=>n==null?'—':n.toFixed(1)+'%';
const age=ms=>ms<60000?Math.floor(ms/1000)+'s':ms<3600000?Math.floor(ms/60000)+'m':ms<86400000?Math.floor(ms/3600000)+'h':Math.floor(ms/86400000)+'d';
const text=(id,value)=>{$(id).textContent=value;};
/* Four-significant-digit mantissa with K/M/G/T/P letters; binary base for
 * capacities, decimal base for rates. A value just below the next unit never
 * rounds up to it. */
const byteUnits=['B','K','M','G','T','P'];
function byteScale(n,base) {
  let unit=0;const limit=base*.99995;
  while(n>=limit&&unit<byteUnits.length-1){n/=base;unit++;}
  return[n,byteUnits[unit]];
}
function byteText(n,base) {
  if(n==null)return '—';
  const[value,unit]=byteScale(n,base);
  return mantissa(value)+unit;
}
function mantissa(value) {
  const scaled=Math.round(value*10)/10;
  return (value>0&&!scaled?'0.1':scaled.toFixed(1));
}
const capacity=n=>byteText(n,1024);
function speed(n) {
  if(n==null)return '—';
  return byteText(n,1000)+'/s';
}
function capacityPair(used,total) {
  const left=capacity(used),right=capacity(total),unit=right.at(-1);
  return (left.endsWith(unit)?left.slice(0,-1):left)+'/'+right;
}
const plots=[
  {id:'cores',fields:[],labels:[]},
  {id:'cpu',fields:[],labels:['user','system','iowait']},
  {id:'pressure',fields:[],labels:['CPU','memory','I/O']},
  {id:'memory',fields:[6],labels:['used']},
  {id:'frequency',fields:[],labels:[]},
  {id:'temp',fields:[8],labels:['CPU']},
  {id:'network',fields:[10,9],labels:['upload','download']}
];
function configurePlots() {
  const n=state.cores,f=state.format;
  interfaceIndex=Math.max(0,state.interfaceNames.indexOf(selectedInterface));
  selectedInterface=state.interfaceNames[interfaceIndex];
  plots[1].fields=[f.user,f.system,f.iowait];plots[2].fields=[f.pressure,f.pressure+1,f.pressure+2];
  plots[3].fields=[showSwap?f.swap:f.memory];plots[3].labels=[showSwap?'swap used':'RAM used'];
  plots[4].fields=state.frequencyIds.map((_,i)=>f.frequency+i);plots[4].labels=state.frequencyIds.map(id=>'CPU '+id+' policy');
  plots[5].fields=[f.temp];
  plots[6].fields=state.interfaces?[f.network+2*interfaceIndex+1,f.network+2*interfaceIndex]:[];
  text('memory-title',showSwap?'Swap (%)':'Memory (%)');
  text('memory-series',showSwap?'swap used':'RAM used');
  text('memory-kind',showSwap?'show RAM':'show swap');
  $('memory-kind').setAttribute('aria-label',showSwap?'Show RAM history':'Show swap history');
  $('memory-kind').setAttribute('aria-pressed',String(showSwap));
  text('network-interface',selectedInterface||'none');
  text('network-series',selectedInterface||'no interfaces');
  $('network-toggle').disabled=state.interfaces<2;
  $('network-toggle').setAttribute('aria-label',state.interfaces>1?'Network '+selectedInterface+'. Show next interface':'Network '+(selectedInterface||'unavailable'));
  const signature=state.cpuIds.join(',');
  if(coreTopology!==signature) {
    plots[0].fields=Array.from({length:n},(_,i)=>i);
    plots[0].labels=state.cpuIds.map(id=>'CPU '+id);
    $('core-legend').innerHTML=state.cpuIds.map((id,i)=>'<span><i style="background:'+seriesColor(i)+'"></i>'+id+'</span>').join(' ');
    coreTopology=signature;
  }
}
function shortSpeed(n) {
  if(n==null)return '—';
  return byteText(n,1000);
}
function renderCoreBars() {
  if(!state||view.value!=='bars')return;
  const list=$('core-bars'),signature=state.cpuIds.join(',');
  if(coreBarsTopology!==signature) {
    coreMeters=state.cpuIds.map((id,k)=>{
      const item=document.createElement('li'),label=document.createElement('span'),bar=document.createElement('meter'),value=document.createElement('span');
      label.className='core-id';label.textContent=id;label.title='CPU '+id;
      bar.className='core-meter';bar.min=0;bar.max=100;bar.style.color=seriesColor(k);
      bar.setAttribute('aria-label','CPU '+id+' usage');
      value.className='core-value';item.append(label,bar,value);
      return {item,bar,value};
    });
    list.style.setProperty('--core-rows',String(Math.ceil(state.cores/2)));
    list.dataset.columns=String(state.cores>1?2:1);
    list.replaceChildren(...coreMeters.map(core=>core.item));coreBarsTopology=signature;
  }
  for(let k=0;k<coreMeters.length;k++) {
    const current=state.latest[k],core=coreMeters[k];
    core.bar.value=current??0;core.bar.dataset.available=String(current!=null);
    core.bar.setAttribute('aria-valuetext',current==null?'unavailable':percent(current));
    core.value.textContent=percent(current);
  }
}
function setCoreView() {
  const bars=view.value==='bars';
  $('core-plot').hidden=bars;$('core-bars').hidden=!bars;$('core-legend').hidden=bars;
  clearInspection();
  if(bars)renderCoreBars();else queueDraw();
}
function formatValue(plot,n) {
  return n==null?'—':plot.id==='network'?speed(n):plot.id==='frequency'?Number((n/1000).toFixed(3))+' GHz':n.toFixed(1)+(plot.id==='temp'?' °C':'%');
}
function summaries() {
  const skipped=historyCache.skipped;
  const r=state.latest,n=state.cores,f=state.format;
  text('overall',percent(r[f.overall]));text('overall-sub',n+' logical '+(n===1?'CPU':'CPUs'));
  text('cpu',percent(r[cpuSystem?f.system:f.user]));text('cpu-mode',cpuSystem?'system':'user');
  $('cpu-toggle').setAttribute('aria-label','CPU time: '+(cpuSystem?'system. Show user time':'user. Show system time'));
  const memoryPercent=r[f.memory]==null?null:memoryFree?100-r[f.memory]:r[f.memory],total=state.memory*1024;
  text('memory',memoryPercent==null?'—':capacity(total*memoryPercent/100));
  text('memory-mode',memoryFree?'free':'used');
  text('memory-sub',percent(memoryPercent)+' of '+capacity(total));
  $('memory-toggle').setAttribute('aria-label','Memory '+(memoryFree?'free. Show used memory':'used. Show free memory'));
  text('swap',state.swap?r[f.swap]==null?'—':capacity(state.swap*1024*r[f.swap]/100):'off');
  text('swap-sub',state.swap?percent(r[f.swap])+' of '+capacity(state.swap*1024):'no swap configured');
  text('temp',formatValue(plots[5],r[f.temp]));
  text('temp-sub',r[f.temp]==null?'sensor unavailable':'thermal sensor');text('sensor-state',r[f.temp]==null?'unavailable':'CPU');
  const network=plots[6].fields;
  text('network','U'+shortSpeed(r[network[0]])+'|D'+shortSpeed(r[network[1]]));
  text('frequency-source',state.frequencies?(state.frequencyHardware.every(Boolean)?'hardware-reported':'driver-reported'):'unavailable');
  const index=state.index;
  text('detail','Sample '+state.interval/1000+'s · '+visible.length+' windows · '+age(state.widths[index]*1000)+'/window · lines: average, bands: min or average to max'+(skipped?' · '+skipped+' damaged records skipped':''));
  status();
  renderCoreBars();
}
let renderedDisks,renderedDiskError;
function renderDisks() {
  const list=$('disks');
  if(disksSnapshot!==renderedDisks||diskError!==renderedDiskError) {
    renderedDisks=disksSnapshot;renderedDiskError=diskError;
    if(!disksSnapshot){list.textContent=diskError?'Capacity data unavailable':'Waiting for capacities…';text('disk-count',diskError?'unavailable':'mounted');return;}
    const rows=disksSnapshot.rows;
    text('disk-count',rows.length+' mounted'+(disksSnapshot.partial?' · partial':''));
    const items=rows.map(row=>{
      const item=document.createElement('li'),label=document.createElement('span'),bar=document.createElement(row.valid?'meter':'span'),values=document.createElement('span'),available=document.createElement('span');
      label.className='disk-label';label.textContent=row.source.replace(/^\/dev\//,'')+' · '+row.mount;label.title=row.source+' · '+row.mount;
      bar.className='disk-meter';values.className='disk-values';available.className='disk-available';
      if(row.valid){
        bar.min=0;bar.max=row.total;bar.value=row.used;
        bar.setAttribute('aria-label',row.source+' used space');
        bar.setAttribute('aria-valuetext',percent(100*row.used/row.total)+' used');
        values.textContent=capacityPair(row.used,row.total);
        values.setAttribute('aria-label',capacity(row.used)+' used of '+capacity(row.total));
        available.textContent=capacity(row.available);
        const caption=document.createElement('small');caption.textContent='avail';available.append(caption);
      }else{
        bar.setAttribute('aria-hidden','true');values.textContent='capacity unavailable';available.textContent='—';
      }
      item.append(label,bar,values,available);return item;
    });
    list.replaceChildren(...items);
    if(!rows.length)list.textContent='No mounted local filesystems';
  }
  if(disksSnapshot)text('disk-status',diskError?'Refresh failed · showing previous capacities':'Updated '+age(Math.max(0,Date.now()-disksSnapshot.generated))+' ago');
}
function serviceAge(seconds) {
  if(seconds>=86400)return Math.floor(seconds/86400)+'d '+Math.floor(seconds%86400/3600)+'h';
  if(seconds>=3600)return Math.floor(seconds/3600)+'h '+Math.floor(seconds%3600/60)+'m';
  if(seconds>=60)return Math.floor(seconds/60)+'m '+seconds%60+'s';
  return seconds+'s';
}
const serviceMemoryFields={
  memory:{label:'Cgroup',description:'Memory charged to the service cgroup, including descendants'},
  rss:{label:'RSS',description:'Resident mapped memory; shared pages counted once per process'},
  pss:{label:'PSS',description:'Mapped memory with shared pages divided among sharing processes'},
  uss:{label:'Private',description:'Private clean and dirty mapped pages; excludes hugetlb',rounded:true},
  anon:{label:'PSS anonymous',rounded:true},
  file:{label:'PSS file',rounded:true,title:'Mapped libraries and other file-backed pages'},
  shmem:{label:'PSS shared memory',rounded:true}
};
const serviceStates=['unavailable','inactive','active','failed','activating','stopping','reloading','unavailable'];
let serviceViews=[],serviceSignature;
function renderServices() {
  const rows=state.services;
  $('services-card').hidden=!rows.length;
  if(!rows.length)return;
  const metric=$('service-memory-mode').value||'pss',selected=serviceMemoryFields[metric];
  text('service-memory-heading',selected.label);
  const sorted=[...rows].sort((a,b)=>a.name.localeCompare(b.name)),signature=sorted.map(row=>row.name+(row.openrc?'\0o':'')).join('\n');
  if(signature!==serviceSignature) {
    serviceSignature=signature;text('service-count',rows.length+' tracked');
    serviceViews=sorted.map(row=>{
      const item=document.createElement('tr'),name=document.createElement('th'),memory=document.createElement('td'),uptime=document.createElement('td');
      name.scope='row';
      const label=document.createElement('span');label.textContent=row.openrc?row.name:row.name.replace(/\.service$/,'');name.title=row.name+(row.openrc?' (OpenRC)':'');
      const details=document.createElement('details'),summary=document.createElement('summary'),list=document.createElement('dl'),values={};
      summary.textContent='Breakdown';
      for(const [field,definition] of Object.entries(serviceMemoryFields)){
        const term=document.createElement('dt');term.textContent=definition.label;
        if(definition.title)term.title=definition.title;
        list.append(term,values[field]=document.createElement('dd'));
      }
      details.append(summary,list);name.append(label,details);item.append(name,memory,uptime);
      return {item,memory,uptime,values};
    });
    $('services').replaceChildren(...serviceViews.map(view=>view.item));
  }
  sorted.forEach((row,i)=>{
    const view=serviceViews[i];
    for(const [field,definition] of Object.entries(serviceMemoryFields)){
      const value=view.values[field];value.textContent=capacity(row[field]);
      value.title=definition.rounded&&row[field]!=null?'Stored rounded to four significant digits':'';
    }
    view.memory.textContent=capacity(row[metric]);view.memory.title=row[metric]==null?'Whole-service '+selected.label+' unavailable':selected.description;
    view.uptime.textContent=row.uptime==null?serviceStates[row.status]:serviceAge(row.uptime);
    view.uptime.title=row.uptime==null?serviceStates[row.status]:row.processAge?'Age of the main service process':'Time since the service became active';
  });
}
function status(error) {
  if(error){statusLine.dataset.state='error';statusLine.textContent=state?'Refresh failed · showing last snapshot':error===noProbes?error.message:'Cannot load data from quectop-hub';statusLine.title=error.message;return;}
  if(!state)return;
  const elapsed=Math.max(0,Date.now()-state.latestTime),stale=elapsed>Math.max(90000,state.interval*3,state.publish*3);
  statusLine.dataset.state=stale?'error':'ok';
  statusLine.textContent='uptime: '+age(state.uptime)+(stale?' · last sample '+age(elapsed)+' ago':' · sampled '+age(elapsed)+' ago');
  statusLine.title=stale?'The device may be offline, or quectop on it may be stopped.':'';
}
const field=(row,part,plot,k)=>row[part][plot.fields[k]];
let colors;
function redraw() {
  if(!state)return;
  const count=Math.max(2,state.cores,state.frequencies);
  if(colors?.theme!==theme||colors.palette.length!==count) {
    const style=getComputedStyle(document.documentElement),lightness=style.getPropertyValue('--series-lightness').trim();
    colors={theme,grid:style.getPropertyValue('--grid'),muted:style.getPropertyValue('--muted'),
      palette:Array.from({length:count},(_,k)=>k<colorNames.length?style.getPropertyValue(colorNames[k]).trim():'hsl('+((k*137.508)%360).toFixed(1)+' 55% '+lightness+')')};
  }
  for(const plot of plots)if(plot.id!=='cores'||view.value!=='bars')draw(plot,colors);
  renderInspection();
}
function draw(plot,colors) {
  const canvas=$('chart-'+plot.id),rect=canvas.getBoundingClientRect(),w=Math.round(rect.width),h=Math.round(rect.height),dpr=Math.min(devicePixelRatio||1,2);
  if(!w||!h)return;
  const pw=Math.round(w*dpr),ph=Math.round(h*dpr),c=canvas.getContext('2d');
  if(canvas.width!==pw||canvas.height!==ph){canvas.width=pw;canvas.height=ph;}
  c.setTransform(dpr,0,0,dpr,0,0);c.clearRect(0,0,w,h);
  const left=60,right=w-8,top=8,bottom=h-32,index=state.index,width=state.widths[index]*1000;
  const end=state.latestTime,start=end-spans[index]*1000;
  let divisor=1,unit='B',largest=0;
  if(plot.id==='network') {
    for(const r of visible)for(const f of plot.fields)largest=Math.max(largest,r.max[f]||0);
    let u=0;while(largest/1000**u>=999.95&&u<byteUnits.length-1)u++;divisor=1000**u;unit=byteUnits[u];
    text('network-title','Network ('+unit+'/s)');
  }
  if(plot.id==='frequency')divisor=1000;
  const isPercent=plot.id!=='temp'&&plot.id!=='network'&&plot.id!=='frequency';
  let max=-Infinity,min=Infinity,hasValue=false;
  for(const r of visible)for(let k=0;k<plot.fields.length;k++) {
    const lo=field(r,'min',plot,k)??field(r,'mean',plot,k),hi=field(r,'max',plot,k);
    if(hi!=null){hasValue=true;max=Math.max(max,hi/divisor);min=Math.min(min,lo/divisor);}
  }
  if(hasValue) {
    const padding=Math.max((max-min)*.1,plot.id==='network'?Math.max(.01/divisor,10**Math.floor(Math.log10(max||1))*.01):plot.id==='frequency'?.05:.5);
    const step=10**Math.floor(Math.log10(max-min+2*padding))/2;
    min=Math.floor((min-padding)/step)*step;max=Math.ceil((max+padding)/step)*step;
    if(plot.id!=='temp')min=Math.max(0,min);
    if(isPercent)max=Math.min(100,max);
  }else{min=0;max=isPercent?100:1;}
  const x=t=>left+(t-start)/(end-start)*(right-left),y=v=>bottom-(v-min)/(max-min)*(bottom-top);
  const axisDigits=Math.max(0,Math.min(9,-Math.floor(Math.log10((max-min)/4))));
  const center=r=>Math.max(start,Math.min(end,r.start+(r.current?Math.max(0,end-r.start)/2:width/2)));
  c.font='14px ui-monospace,monospace';c.lineWidth=.6;c.textBaseline='middle';
  for(let i=0;i<=4;i++) {
    const value=min+(max-min)*i/4,py=y(value);
    c.strokeStyle=colors.grid;c.beginPath();c.moveTo(left,py);c.lineTo(right,py);c.stroke();
    c.fillStyle=colors.muted;c.textAlign='right';c.fillText(plot.id==='network'?mantissa(value)+unit:Number(value.toFixed(axisDigits)).toString(),left-6,py);
  }
  const ticks=w<380?3:5;c.textAlign='center';
  for(let i=0;i<ticks;i++) {
    const t=start+(end-start)*i/(ticks-1),px=x(t);
    c.strokeStyle=colors.grid;c.beginPath();c.moveTo(px,top);c.lineTo(px,bottom);c.stroke();
    c.fillStyle=colors.muted;const date=new Date(t);
    const label=index>1?date.toLocaleDateString(undefined,{month:'short',day:'numeric'}):date.toLocaleTimeString(undefined,{hour:'2-digit',minute:'2-digit',hour12:false}),half=c.measureText(label).width/2;
    c.fillText(label,Math.max(left+half,Math.min(right-half,px)),h-12);
  }
  c.save();c.beginPath();c.rect(left,top,right-left,bottom-top);c.clip();
  for(let k=0;k<plot.fields.length;k++) {
    c.strokeStyle=c.fillStyle=colors.palette[k];
    let segment=[];
    const paint=()=>{
      if(!segment.length)return;
      c.globalAlpha=.12;c.beginPath();
      segment.forEach((r,i)=>{const px=x(center(r)),py=y(field(r,'max',plot,k)/divisor);if(i)c.lineTo(px,py);else c.moveTo(px,py);});
      for(let i=segment.length-1;i>=0;i--){const r=segment[i];c.lineTo(x(center(r)),y((field(r,'min',plot,k)??field(r,'mean',plot,k))/divisor));}
      c.closePath();c.fill();
      // Exact peak times locate brief excursions within each averaging window.
      c.globalAlpha=.5;c.lineWidth=.8;c.beginPath();
      for(const r of segment) {
        const f=plot.fields[k],px=x(Math.max(start,Math.min(end,r.peak[f]))),mean=field(r,'mean',plot,k)/divisor;
        c.moveTo(px,y(mean));c.lineTo(px,y(field(r,'max',plot,k)/divisor));
      }
      c.stroke();c.globalAlpha=1;c.lineWidth=1.2;c.beginPath();
      segment.forEach((r,i)=>{const px=x(center(r)),py=y(field(r,'mean',plot,k)/divisor);if(i)c.lineTo(px,py);else c.moveTo(px,py);});c.stroke();
      if(segment.length===1){const r=segment[0];c.beginPath();c.arc(x(center(r)),y(field(r,'mean',plot,k)/divisor),2,0,Math.PI*2);c.fill();}
      segment=[];
    };
    let previous=0;
    for(const r of visible){if(field(r,'mean',plot,k)==null||previous&&r.start-previous>width*1.5)paint();if(field(r,'mean',plot,k)!=null)segment.push(r);previous=r.start;}
    paint();
  }
  c.restore();
  if(!hasValue){c.fillStyle=colors.muted;c.textAlign='center';c.fillText(plot.id==='temp'?'Sensor unavailable':plot.id==='pressure'&&plot.fields.every(f=>state.latest[f]==null)?'Pressure stall information unavailable':plot.id==='frequency'?'CPU frequency unavailable':plot.id==='memory'&&showSwap&&!state.swap?'No swap configured':'Waiting for samples',(left+right)/2,(top+bottom)/2);}
  plot.geometry={w,h,left,right,top,bottom,width,start,end,x,y,center,divisor};
  plot.tipKey='';
  ensureMarkers(plot);
  canvas.setAttribute('aria-label',plot.id+' history: average lines and minimum/maximum bands. Latest: '+plot.fields.map((f,k)=>plot.labels[k]+' '+formatValue(plot,state.latest[f])).join(', '));
}
function queueDraw(){if(!frame)frame=requestAnimationFrame(()=>{frame=0;redraw();});}
async function request(name) {
  const response=await fetch(name,{cache:'no-cache',signal:AbortSignal.timeout(10000)});
  if(!response.ok)throw Error(name+': HTTP '+response.status);
  return response;
}
const download=async name=>(await request('h/'+encodeURIComponent(hostId)+'/'+name)).arrayBuffer();
/* Hub lines: id, connected (0/1), hostname. The URL hash selects a host. */
async function loadHosts() {
  const hosts=(await (await request('hosts.txt')).text()).split('\n').filter(Boolean).map(line=>{const[id,connected,name]=line.split('\t');return{id,connected:connected==='1',name:name||id};});
  const wanted=hostId||decodeURIComponent(location.hash.slice(1));
  hostId=(hosts.find(h=>h.id===wanted)||hosts.find(h=>h.connected)||hosts[0])?.id||'';
  hostSelect.replaceChildren(...hosts.map(h=>{const option=document.createElement('option');option.value=h.id;option.textContent=h.name+(h.connected?'':' (offline)');option.selected=h.id===hostId;return option;}));
  hostSelect.hidden=hosts.length<2;
}
async function refresh() {
  clearTimeout(pollTimer);if(document.hidden||busy)return;busy=true;
  const index=Number(range.value);
  try {
    if(!hostId){await loadHosts();if(!hostId)throw noProbes;}
    const next=decodeLive(await download(liveFile));next.index=index;
    const active=next.active[index],key=index+':'+next.points+':'+next.interval+':'+next.metadataHash+':'+next.frequencyBits+':'+next.committed[index];
    if(!historyCache||historyCache.key!==key){const data=decodeHistory(await download(files[index]),index,next);historyCache={key,data,skipped:damagedRecords};}
    if(index!==Number(range.value))return;
    const cutoff=next.latestTime-spans[index]*1000,width=next.widths[index]*1000;
    const recent=index===0?next.pending.slice():[];if(active)recent.push(active);
    const first=recent.length?recent[0].start:Infinity,newest=active?.start??next.latestTime,rows=[];
    for(const r of historyCache.data)if(r.start+width>cutoff&&r.start<first&&r.start<=newest)rows.push(r);
    for(const r of recent)if(r.start+width>cutoff)rows.push(r);
    visible=rows.length>next.points?rows.slice(-next.points):rows;
    if(next.hostname!==state?.hostname){text('host',next.hostname);document.title=(next.hostname?next.hostname+' · ':'')+'quectop';}
    state=next;configurePlots();summaries();renderServices();queueDraw();
    if(!disksSnapshot||disksSnapshot.generated!==next.disksGenerated){
      try{const snapshot=decodeDisks(await download(disksFile));if(snapshot.generated>=next.disksGenerated){disksSnapshot=snapshot;diskError='';}}
      catch(error){diskError=error.message;}
    }
    renderDisks();
  }catch(error){if(state&&Number(range.value)===index)range.value=String(state.index);status(error);}
  finally{busy=false;if(!document.hidden)pollTimer=setTimeout(refresh,index!==Number(range.value)?0:15000);}
}
$('cpu-toggle').addEventListener('click',()=>{cpuSystem=!cpuSystem;if(state)summaries();});
$('memory-toggle').addEventListener('click',()=>{memoryFree=!memoryFree;if(state)summaries();});
$('memory-kind').addEventListener('click',()=>{showSwap=!showSwap;clearInspection();if(state){configurePlots();queueDraw();}});
$('service-memory-mode').addEventListener('change',()=>{if(state)renderServices();});
$('network-toggle').addEventListener('click',()=>{if(!state||state.interfaces<2)return;selectedInterface=state.interfaceNames[(interfaceIndex+1)%state.interfaces];clearInspection();configurePlots();summaries();queueDraw();});
range.addEventListener('change',()=>{clearInspection();refresh();});
hostSelect.addEventListener('focus',()=>loadHosts().catch(()=>{}));
hostSelect.addEventListener('change',()=>{location.hash=encodeURIComponent(hostSelect.value);});
addEventListener('hashchange',()=>location.reload());
view.addEventListener('change',setCoreView);
setCoreView();
let theme;try{theme=localStorage.getItem('quectop-theme');}catch{}
if(theme!=='dark'&&theme!=='light')theme=matchMedia('(prefers-color-scheme: dark)').matches?'dark':'light';
function setTheme(){document.documentElement.dataset.theme=theme;$('theme').setAttribute('aria-label','Use '+(theme==='light'?'dark':'light')+' theme');queueDraw();}
setTheme();$('theme').addEventListener('click',()=>{theme=theme==='light'?'dark':'light';try{localStorage.setItem('quectop-theme',theme);}catch{}setTheme();});
new ResizeObserver(queueDraw).observe(document.querySelector('.charts'));
document.addEventListener('visibilitychange',()=>{if(document.hidden)clearTimeout(pollTimer);else refresh();});
function ensureMarkers(plot) {
  const signature=plot.fields.join(',');
  if(plot.markerKey===signature)return;
  plot.markers=plot.fields.map((_,k)=>{
    const marker=document.createElement('i');marker.className='graph-dot';marker.hidden=true;
    marker.style.background=seriesColor(k);return marker;
  });
  $('dots-'+plot.id).replaceChildren(...plot.markers);plot.markerKey=signature;
}
function queueInspection(){if(!inspectionFrame)inspectionFrame=requestAnimationFrame(()=>{inspectionFrame=0;renderInspection();});}
function clearInspection(){inspection=null;queueInspection();}
function detailText(plot,row) {
  const fmt=n=>n==null?'unavailable':formatValue(plot,n);
  return new Date(row.start).toLocaleString()+' · '+plot.fields.map((f,k)=>{
    const min=field(row,'min',plot,k),max=fmt(field(row,'max',plot,k));
    return plot.labels[k]+' average '+fmt(field(row,'mean',plot,k))+(min==null?', maximum '+max:', range '+fmt(min)+'–'+max)+', peak '+new Date(row.peak[f]).toLocaleTimeString();
  }).join(' · ');
}
function renderInspection() {
  const row=inspection&&visible.find(r=>r.start===inspection.start);
  if(!row)inspection=null;
  for(const plot of plots) {
    const guide=$('guide-'+plot.id),tip=$('tip-'+plot.id),geometry=plot.geometry;
    const shown=row&&(plot.id!=='cores'||view.value!=='bars');
    guide.hidden=!shown;tip.hidden=true;
    if(!shown||!geometry){for(const marker of plot.markers||[])marker.hidden=true;continue;}
    const px=geometry.x(geometry.center(row));
    guide.style.left=px+'px';guide.style.top=geometry.top+'px';guide.style.height=(geometry.bottom-geometry.top)+'px';
    for(let k=0;k<plot.fields.length;k++) {
      const marker=plot.markers[k],value=field(row,'mean',plot,k);
      marker.hidden=value==null;
      if(value!=null){marker.style.left=px+'px';marker.style.top=Math.max(geometry.top,Math.min(geometry.bottom,geometry.y(value/geometry.divisor)))+'px';}
    }
    if(inspection.id!==plot.id)continue;
    const key=row.start+':'+state.generated+':'+plot.fields.join(',');
    tip.hidden=false;
    if(plot.tipKey!==key) {
      const fmt=n=>formatValue(plot,n);
      tip.innerHTML='<time>'+inspectionDate.format(new Date(geometry.center(row)))+'</time><div class="tip-values'+(plot.fields.length>8?' tip-columns':'')+'">'+plot.fields.map((_,k)=>'<div class="tip-row"><i style="background:'+seriesColor(k)+'"></i><span>'+plot.labels[k]+'</span><b>'+fmt(field(row,'mean',plot,k))+'</b></div>').join('')+'</div>';
      $('detail-'+plot.id).textContent=detailText(plot,row);
      plot.tipKey=key;plot.tipWidth=tip.offsetWidth;plot.tipHeight=tip.offsetHeight;
    }
    const top=inspection.y??geometry.top+12;
    tip.style.left=Math.max(6,Math.min(geometry.w-plot.tipWidth-6,px+12+plot.tipWidth>geometry.w-6?px-plot.tipWidth-12:px+12))+'px';
    tip.style.top=Math.max(6,Math.min(geometry.h-plot.tipHeight-6,top-12))+'px';
  }
}
for(const plot of plots) {
  const canvas=$('chart-'+plot.id);
  function pointer(event) {
    if(!visible.length||!plot.geometry)return;
    const g=plot.geometry,rect=canvas.getBoundingClientRect(),px=event.clientX-rect.left,py=event.clientY-rect.top;
    if(px<g.left||px>g.right||py<g.top||py>g.bottom){clearInspection();return;}
    const time=g.start+(px-g.left)/(g.right-g.left)*(g.end-g.start);
    let low=0,high=visible.length-1;
    while(low<high){const mid=(low+high)>>1;if(g.center(visible[mid])<time)low=mid+1;else high=mid;}
    if(low&&Math.abs(g.center(visible[low-1])-time)<Math.abs(g.center(visible[low])-time))low--;
    if(Math.abs(g.center(visible[low])-time)>g.width*1.5){clearInspection();return;}
    inspection={id:plot.id,start:visible[low].start,y:py,keyboard:false};queueInspection();
  }
  canvas.addEventListener('pointermove',pointer);canvas.addEventListener('pointerdown',pointer);
  canvas.addEventListener('pointercancel',clearInspection);
  canvas.parentElement.addEventListener('pointerleave',()=>{if(document.activeElement!==canvas)clearInspection();});
  canvas.addEventListener('focus',()=>{if(visible.length&&inspection?.id!==plot.id){inspection={id:plot.id,start:visible[visible.length-1].start,keyboard:true};queueInspection();}});
  canvas.addEventListener('blur',()=>{if(inspection?.id===plot.id)clearInspection();});
  canvas.addEventListener('keydown',event=>{
    if(event.key==='Escape'){event.preventDefault();clearInspection();return;}
    if(inspection?.id===plot.id&&['PageUp','PageDown'].includes(event.key)) {
      event.preventDefault();const tip=$('tip-'+plot.id);
      tip.scrollTop+=(event.key==='PageDown'?1:-1)*tip.clientHeight*.8;return;
    }
    if(!visible.length||!['ArrowLeft','ArrowRight','Home','End'].includes(event.key))return;
    event.preventDefault();let cursor=inspection?visible.findIndex(r=>r.start===inspection.start):visible.length-1;
    if(cursor<0)cursor=visible.length-1;
    if(event.key==='Home')cursor=0;else if(event.key==='End')cursor=visible.length-1;
    else cursor=Math.max(0,Math.min(visible.length-1,cursor+(event.key==='ArrowRight'?1:-1)));
    inspection={id:plot.id,start:visible[cursor].start,keyboard:true};queueInspection();
  });
}
refresh();
