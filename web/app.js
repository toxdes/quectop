'use strict';
const $=id=>document.getElementById(id);
const range=$('range'),view=$('view'),statusLine=$('status');
const schemaVersion=7,diskSchemaVersion=2;
const spans=[3600,86400,604800,2592000,7776000],files=['1h','1d','7d','30d','90d'].map(name=>name+'.v'+schemaVersion+'.bin');
const liveFile='live.v'+schemaVersion+'.bin',disksFile='disks.v'+diskSchemaVersion+'.bin';
const colorNames=['--teal','--violet','--third','--fourth','--core5','--core6','--core7','--core8'];
const seriesColor=k=>k<colorNames.length?'var('+colorNames[k]+')':'hsl('+((k*137.508)%360).toFixed(1)+' 55% var(--series-lightness))';
let coreTopology='',cpuSystem=false,memoryFree=false,showSwap=false,interfaceIndex=0,selectedInterface;
let coreBarsTopology='',coreMeters=[];
let disksSnapshot,diskError='';
let state,visible=[],historyCache,pollTimer,busy=false,frame=0,skipped=0,inspection=null,inspectionFrame=0;
const inspectionDate=new Intl.DateTimeFormat(undefined,{year:"numeric",month:"short",day:"numeric",hour:"2-digit",minute:"2-digit",second:"2-digit"});
const percent=n=>n==null?'—':n.toFixed(1)+'%';
const age=ms=>ms<60000?Math.floor(ms/1000)+'s':ms<3600000?Math.floor(ms/60000)+'m':ms<86400000?Math.floor(ms/3600000)+'h':Math.floor(ms/86400000)+'d';
const text=(id,value)=>{$(id).textContent=value;};
function capacity(n) {
  if(n==null)return '—';
  let unit=0;while(n>=1024&&unit<4){n/=1024;unit++;}
  return Number(n.toPrecision(3))+' '+['B','KiB','MiB','GiB','TiB'][unit];
}
function capacityPair(used,total) {
  const left=capacity(used),right=capacity(total),unit=right.slice(right.lastIndexOf(' '));
  return (left.endsWith(unit)?left.slice(0,-unit.length):left)+'/'+right;
}
function speed(n) {
  if(n==null)return '—';
  let unit=0;while(n>=999.5&&unit<3){n/=1000;unit++;}
  return n.toFixed(n>=100?0:n>=10?1:2)+' '+['B','KB','MB','GB'][unit]+'/s';
}
const plots=[
  {id:'cores',fields:[],labels:[]},
  {id:'cpu',fields:[4,5],labels:['user','system']},
  {id:'memory',fields:[6],labels:['used']},
  {id:'frequency',fields:[],labels:[]},
  {id:'temp',fields:[8],labels:['CPU']},
  {id:'network',fields:[10,9],labels:['upload','download']}
];
function magic(d,value) {
  for(let i=0;i<8;i++)if(d.getUint8(i)!==(value.charCodeAt(i)||0))return false;
  return true;
}
function checksum(bytes,at,length,h=2166136261) {
  for(let i=at;i<at+length;i++)h=Math.imul(h^bytes[i],16777619);
  return h>>>0;
}
function validHash(d,at,length) {
  return checksum(new Uint8Array(d.buffer,d.byteOffset,d.byteLength),at,length-4)===d.getUint32(at+length-4,true);
}
function reader(d,at) {
  let position=at*8;
  return n=>{
    let value=0,factor=1;
    while(n){const shift=position&7,take=Math.min(n,8-shift);value+=((d.getUint8(position>>3)>>shift)&((1<<take)-1))*factor;factor*=2**take;position+=take;n-=take;}
    return value;
  };
}
function layout(cores,frequencies,frequencyBits,interfaces) {
  const temp=cores+5,frequency=cores+6,network=frequency+frequencies;
  const bits=Array(temp).fill(10).concat(12,Array(frequencies).fill(frequencyBits),Array(interfaces*2).fill(14));
  return {cores,bits,metrics:bits.length,valueBits:10*temp+12+frequencies*frequencyBits+28*interfaces,temp,frequency,network};
}
function unpackValues(read,valid,format) {
  return format.bits.map((width,k)=>{
    const code=read(width);
    if(!valid[k])return null;
    if(k<format.temp){if(code>1000)throw Error('Invalid percentage');return code/10;}
    if(k===format.temp)return (code-2048)/10;
    if(k<format.network)return code;
    const mantissa=code&1023,decimals=(code>>10)&3,unit=(code>>12)&3;
    if(mantissa>999||decimals>2)throw Error('Invalid network rate');
    return mantissa*1000**unit/10**decimals;
  });
}
function recordBytes(width,format){return Math.ceil((32+format.metrics+3*format.valueBits+format.metrics*Math.max(1,Math.ceil(Math.log2(width))))/8)+4;}
function decodeBucket(d,at,width,bytes,format) {
  if(!d.getUint32(at,true))return null;
  if(!validHash(d,at,bytes)){skipped++;return null;}
  const read=reader(d,at),start=read(32)*1000,valid=format.bits.map(()=>read(1));
  if(!valid.some(Boolean))return null;
  if(start/1000%width)throw Error('Misaligned bucket');
  const mean=unpackValues(read,valid,format),min=unpackValues(read,valid,format),max=unpackValues(read,valid,format);
  const offsetBits=Math.max(1,Math.ceil(Math.log2(width)));
  const peak=format.bits.map(()=>start+read(offsetBits)*1000);
  for(let k=0;k<format.metrics;k++)if(mean[k]!=null&&(mean[k]<min[k]||mean[k]>max[k]||peak[k]>=start+width*1000))throw Error('Invalid bucket values');
  return {start,mean,min,max,peak};
}
function decodeLive(buffer) {
  const d=new DataView(buffer);
  if(d.byteLength<148||!magic(d,'QUECTOPL')||d.getUint32(8,true)!==schemaVersion||!validHash(d,0,d.byteLength))throw Error('Invalid '+liveFile+' (expected schema v'+schemaVersion+')');
  const points=d.getUint32(12,true),interval=d.getUint32(16,true),publish=d.getUint32(20,true);
  const cores=d.getUint32(52,true),idBits=d.getUint32(56,true),metrics=d.getUint32(60,true);
  const interfaces=d.getUint32(108,true),frequencies=d.getUint32(120,true),frequencyBits=d.getUint32(124,true),metadataBytes=d.getUint32(128,true);
  if(points<16||points>10000||interval<100||!publish||!cores||cores>1048576||interfaces>65535||frequencies>65535||frequencies>cores||frequencyBits<1||frequencyBits>16||metrics!==cores+6+frequencies+2*interfaces||idBits<1||idBits>32)throw Error('Invalid live configuration');
  const format=layout(cores,frequencies,frequencyBits,interfaces),idBytes=Math.ceil((cores*idBits+frequencies*(idBits+1))/8),latestBytes=Math.ceil((format.metrics+format.valueBits)/8);
  if(metadataBytes<idBytes||metadataBytes>idBytes+interfaces*16)throw Error('Invalid device metadata');
  const widths=spans.map((span,i)=>{const width=d.getUint32(64+i*4,true),expected=Math.max(Math.ceil(span/(points-1)),i===0?Math.ceil(interval/1000):1);if(width!==expected)throw Error('Invalid bucket width');return width;});
  const sizes=widths.map(width=>recordBytes(width,format)),pendingCount=d.getUint32(104,true),serviceCount=d.getUint32(132,true);
  const capacity=Math.max(1,Math.floor(1024/sizes[0]));
  if(serviceCount>65535||pendingCount>capacity||d.byteLength<148+metadataBytes+latestBytes+sizes.reduce((a,b)=>a+b,0)+pendingCount*sizes[0])throw Error('Incomplete '+liveFile);
  const ids=reader(d,144),cpuIds=Array.from({length:cores},()=>ids(idBits));
  if(cpuIds.some((id,i)=>i&&id<=cpuIds[i-1]))throw Error('Invalid CPU IDs');
  const frequencyIds=[],frequencyHardware=[];
  for(let i=0;i<frequencies;i++){frequencyIds.push(ids(idBits));frequencyHardware.push(Boolean(ids(1)));}
  const cpuSet=new Set(cpuIds);
  if(frequencyIds.some((id,i)=>!cpuSet.has(id)||i&&id<=frequencyIds[i-1]))throw Error('Invalid frequency policy IDs');
  const decoder=new TextDecoder(),interfaceNames=[];
  let at=144+idBytes;
  for(let i=0;i<interfaces;i++){
    const length=d.getUint8(at++);
    if(!length||length>15||at+length>144+metadataBytes)throw Error('Invalid interface name');
    interfaceNames.push(decoder.decode(new Uint8Array(buffer,at,length)));at+=length;
  }
  if(at!==144+metadataBytes)throw Error('Invalid metadata length');
  const metadata=new Uint8Array(buffer,144,metadataBytes),metadataHash=checksum(metadata,0,metadataBytes);
  const read=reader(d,at),valid=format.bits.map(()=>read(1)),latest=unpackValues(read,valid,format);
  at+=latestBytes;
  const active=widths.map((width,i)=>{const row=decodeBucket(d,at,width,sizes[i],format);at+=sizes[i];if(row)row.current=true;return row;});
  const pending=[];
  for(let i=0;i<pendingCount;i++){const row=decodeBucket(d,at,widths[0],sizes[0],format);at+=sizes[0];if(row)pending.push(row);}
  const services=[],end=d.byteLength-4;
  const varint=()=>{
    let value=0;
    for(let i=0;i<10&&at<end;i++){
      const byte=d.getUint8(at++);
      if(i===9&&byte>1)break;
      value+=(byte&127)*2**(7*i);
      if(!(byte&128)){if(i&&byte===0)break;return value;}
    }
    throw Error('Invalid service counter');
  };
  for(let i=0;i<serviceCount;i++){
    if(at+2>end)throw Error('Incomplete service entry');
    const length=d.getUint8(at++),flags=d.getUint8(at++);
    if(!length||flags>31||at+length>end||(flags&16)&&![2,6].includes(flags&7))throw Error('Invalid service entry');
    const name=decoder.decode(new Uint8Array(buffer,at,length));at+=length;
    services.push({name,status:flags&7,memory:flags&8?varint():null,uptime:flags&16?varint():null});
  }
  if(at!==end)throw Error('Invalid live snapshot length');
  return {points,interval,publish,cores,cpuIds,idBits,idBytes,metadataBytes,metadata,metadataHash,interfaces,interfaceNames,frequencies,frequencyBits,frequencyIds,frequencyHardware,format,widths,sizes,active,latest,pending,services,
    committed:spans.map((_,i)=>d.getUint32(84+i*4,true)),
    generated:Number(d.getBigUint64(24,true)),uptime:Number(d.getBigUint64(32,true)),memory:Number(d.getBigUint64(40,true)),latestTime:d.getUint32(48,true)*1000,
    swap:Number(d.getBigUint64(112,true)),disksGenerated:Number(d.getBigUint64(136,true))};
}
function decodeHistory(buffer,index,next) {
  const d=new DataView(buffer),bytes=next.sizes[index],width=next.widths[index],header=64+next.metadataBytes;
  if(d.byteLength!==header+(next.points-1)*bytes||!magic(d,'QUECTOPH')||d.getUint32(8,true)!==schemaVersion||d.getUint32(12,true)!==next.points||d.getUint32(16,true)!==spans[index]||d.getUint32(20,true)!==width||d.getUint32(24,true)!==next.interval||d.getUint32(28,true)!==bytes||d.getUint32(32,true)!==index||d.getUint32(36,true)!==next.cores||d.getUint32(40,true)!==next.format.metrics||d.getUint32(44,true)!==header||d.getUint32(48,true)!==next.idBits||d.getUint32(52,true)!==next.interfaces||d.getUint16(56,true)!==next.frequencies||d.getUint8(58)!==next.frequencyBits||d.getUint8(59)!==0)throw Error('History configuration mismatch');
  const raw=new Uint8Array(buffer);
  if(checksum(raw,64,next.metadataBytes,checksum(raw,0,60))!==d.getUint32(60,true))throw Error('Invalid history header');
  if(next.metadata.some((byte,i)=>byte!==raw[64+i]))throw Error('History device metadata mismatch');
  const rows=[];
  for(let at=header;at<d.byteLength;at+=bytes){const row=decodeBucket(d,at,width,bytes,next.format);if(row)rows.push(row);}
  return rows.sort((a,b)=>a.start-b.start);
}
function decodeDisks(buffer) {
  const d=new DataView(buffer);
  if(d.byteLength<32||!magic(d,'QUECTOPD')||d.getUint32(8,true)!==diskSchemaVersion||!validHash(d,0,d.byteLength))throw Error('Invalid '+disksFile);
  const count=d.getUint32(20,true),flags=d.getUint32(24,true),rows=[],decoder=new TextDecoder();
  if(count>65535||flags>3)throw Error('Invalid filesystem snapshot');
  let at=28;
  for(let i=0;i<count;i++){
    const sourceBytes=d.getUint16(at,true),mountBytes=d.getUint16(at+2,true),flags=d.getUint8(at+4),valid=flags&1,bits=(flags>>1)+1,read=reader(d,at+5);
    const total=read(bits)*1024,used=read(bits)*1024,available=read(bits)*1024;at+=5+Math.ceil(3*bits/8);
    if(!sourceBytes||!mountBytes||flags&128||at+sourceBytes+mountBytes>d.byteLength-4||used>total||available>total-used)throw Error('Invalid filesystem capacity');
    const source=decoder.decode(new Uint8Array(buffer,at,sourceBytes));at+=sourceBytes;
    const mount=decoder.decode(new Uint8Array(buffer,at,mountBytes));at+=mountBytes;
    rows.push({source,mount,valid,total,used,available});
  }
  if(at!==d.byteLength-4)throw Error('Invalid filesystem snapshot length');
  return {generated:Number(d.getBigUint64(12,true)),rows,partial:Boolean(flags)};
}
function configurePlots() {
  const n=state.cores;
  interfaceIndex=Math.max(0,state.interfaceNames.indexOf(selectedInterface));
  selectedInterface=state.interfaceNames[interfaceIndex];
  plots[1].fields=[n+1,n+2];plots[2].fields=[n+(showSwap?4:3)];plots[2].labels=[showSwap?'swap used':'RAM used'];
  plots[3].fields=state.frequencyIds.map((_,i)=>state.format.frequency+i);plots[3].labels=state.frequencyIds.map(id=>'CPU '+id+' policy');
  plots[4].fields=[n+5];
  plots[5].fields=state.interfaces?[state.format.network+2*interfaceIndex+1,state.format.network+2*interfaceIndex]:[];
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
  let unit=0;while(n>=999.5&&unit<3){n/=1000;unit++;}
  return Number(n.toPrecision(3))+['B','K','M','G'][unit];
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
  const r=state.latest,n=state.cores;
  text('overall',percent(r[n]));text('overall-sub',n+' logical '+(n===1?'CPU':'CPUs'));
  text('cpu',percent(r[n+(cpuSystem?2:1)]));text('cpu-mode',cpuSystem?'system':'user');
  $('cpu-toggle').setAttribute('aria-label','CPU time: '+(cpuSystem?'system. Show user time':'user. Show system time'));
  const memoryPercent=r[n+3]==null?null:memoryFree?100-r[n+3]:r[n+3],total=state.memory*1024;
  text('memory',memoryPercent==null?'—':capacity(total*memoryPercent/100));
  text('memory-mode',memoryFree?'free':'used');
  text('memory-sub',percent(memoryPercent)+' of '+capacity(total));
  $('memory-toggle').setAttribute('aria-label','Memory '+(memoryFree?'free. Show used memory':'used. Show free memory'));
  $('memory-toggle').title='Free means MemAvailable, including reclaimable cache. Capacities are estimated from the stored 0.1% reading.';
  text('swap',state.swap?r[n+4]==null?'—':capacity(state.swap*1024*r[n+4]/100):'off');
  text('swap-sub',state.swap?percent(r[n+4])+' of '+capacity(state.swap*1024):'no swap configured');
  text('temp',formatValue(plots[4],r[n+5]));
  text('temp-sub',r[n+5]==null?'sensor unavailable':'thermal sensor');text('sensor-state',r[n+5]==null?'unavailable':'CPU');
  const network=plots[5].fields;
  text('network','U'+shortSpeed(r[network[0]])+'|D'+shortSpeed(r[network[1]]));
  text('network-sub','per second');
  $('network-toggle').title='U: upload; D: download. Rates per second. K = KB, M = MB, G = GB; decimal units.';
  text('frequency-source',state.frequencies?(state.frequencyHardware.every(Boolean)?'hardware-reported':'driver-reported'):'unavailable');
  $('frequency-source').title='One reading per shared-clock policy; labels identify its leading CPU. Driver-reported clocks may differ from instantaneous hardware clocks.';
  const index=state.index;
  text('detail','Sample '+state.interval/1000+'s · '+visible.length+' windows · '+age(state.widths[index]*1000)+'/window · lines: average, bands: min/max'+(skipped?' · '+skipped+' damaged records skipped':''));
  status();
  renderCoreBars();
}
function renderDisks() {
  const list=$('disks');
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
  text('disk-status',diskError?'Refresh failed · showing previous capacities':'Updated '+age(Math.max(0,Date.now()-disksSnapshot.generated))+' ago');
}
function serviceAge(seconds) {
  if(seconds>=86400)return Math.floor(seconds/86400)+'d '+Math.floor(seconds%86400/3600)+'h';
  if(seconds>=3600)return Math.floor(seconds/3600)+'h '+Math.floor(seconds%3600/60)+'m';
  if(seconds>=60)return Math.floor(seconds/60)+'m '+seconds%60+'s';
  return seconds+'s';
}
function renderServices() {
  const rows=state.services;
  $('services-card').hidden=!rows.length;
  if(!rows.length)return;
  text('service-count',rows.length+' tracked');
  const states=['unavailable','inactive','active','failed','activating','stopping','reloading','unavailable'];
  const items=rows.map(row=>{
    const item=document.createElement('tr'),name=document.createElement('th'),memory=document.createElement('td'),uptime=document.createElement('td');
    name.scope='row';name.textContent=row.name.replace(/\.service$/,'');name.title=row.name;
    memory.textContent=capacity(row.memory);memory.title=row.memory==null?'Memory accounting unavailable':'systemd MemoryCurrent; includes the service cgroup';
    uptime.textContent=row.uptime==null?states[row.status]:serviceAge(row.uptime);
    uptime.title=row.uptime==null?states[row.status]:'Time since the service became active';
    item.append(name,memory,uptime);return item;
  });
  $('services').replaceChildren(...items);
}
function status(error) {
  if(error){statusLine.dataset.state='error';statusLine.textContent=state?'Refresh failed · showing last snapshot':'Cannot load data · serve this page with the .bin files';statusLine.title=error.message;return;}
  if(!state)return;
  const elapsed=Math.max(0,Date.now()-state.latestTime),stale=elapsed>Math.max(90000,state.interval*3,state.publish*3);
  statusLine.dataset.state=stale?'error':'ok';
  statusLine.textContent='uptime: '+age(state.uptime)+(stale?' · last sample '+age(elapsed)+' ago':' · sampled '+age(elapsed)+' ago');
  statusLine.title=stale?'Collector may be stopped.':'';
}
function field(row,part,plot,k) {
  const value=row[part][plot.fields[k]];
  return value==null?null:plot.invert?.[k]?100-value:value;
}
function redraw() {
  if(!state)return;
  const style=getComputedStyle(document.documentElement),palette=Array.from({length:Math.max(2,state.cores,state.frequencies)},(_,k)=>k<colorNames.length?style.getPropertyValue(colorNames[k]).trim():'hsl('+((k*137.508)%360).toFixed(1)+' 55% '+style.getPropertyValue('--series-lightness').trim()+')');
  for(const plot of plots)if(plot.id!=='cores'||view.value!=='bars')draw(plot,style,palette);
  renderInspection();
}
function draw(plot,style,palette) {
  const canvas=$('chart-'+plot.id),rect=canvas.getBoundingClientRect(),w=Math.round(rect.width),h=Math.round(rect.height),dpr=Math.min(devicePixelRatio||1,2);
  if(!w||!h)return;
  canvas.width=Math.round(w*dpr);canvas.height=Math.round(h*dpr);
  const c=canvas.getContext('2d');c.scale(dpr,dpr);
  const left=60,right=w-8,top=8,bottom=h-32,index=state.index,width=state.widths[index]*1000;
  const end=state.latestTime,start=end-spans[index]*1000;
  let divisor=1,unit='B',largest=0;
  if(plot.id==='network') {
    for(const r of visible)for(const f of plot.fields)largest=Math.max(largest,r.max[f]||0);
    let u=0;while(largest/divisor>=1000&&u<3){divisor*=1000;u++;}unit=['B','KB','MB','GB'][u];
    text('network-title','Network ('+unit+'/s)');
  }
  if(plot.id==='frequency')divisor=1000;
  const isPercent=plot.id!=='temp'&&plot.id!=='network'&&plot.id!=='frequency';
  let max=-Infinity,min=Infinity,hasValue=false;
  for(const r of visible)for(let k=0;k<plot.fields.length;k++) {
    const lo=field(r,plot.invert?.[k]?'max':'min',plot,k),hi=field(r,plot.invert?.[k]?'min':'max',plot,k);
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
    c.strokeStyle=style.getPropertyValue('--grid');c.beginPath();c.moveTo(left,py);c.lineTo(right,py);c.stroke();
    c.fillStyle=style.getPropertyValue('--muted');c.textAlign='right';c.fillText(Number(value.toFixed(axisDigits)).toString(),left-6,py);
  }
  const ticks=w<380?3:5;c.textAlign='center';
  for(let i=0;i<ticks;i++) {
    const t=start+(end-start)*i/(ticks-1),px=x(t);
    c.strokeStyle=style.getPropertyValue('--grid');c.beginPath();c.moveTo(px,top);c.lineTo(px,bottom);c.stroke();
    c.fillStyle=style.getPropertyValue('--muted');const date=new Date(t);
    const label=index>1?date.toLocaleDateString(undefined,{month:'short',day:'numeric'}):date.toLocaleTimeString(undefined,{hour:'2-digit',minute:'2-digit',hour12:false}),half=c.measureText(label).width/2;
    c.fillText(label,Math.max(left+half,Math.min(right-half,px)),h-12);
  }
  c.save();c.beginPath();c.rect(left,top,right-left,bottom-top);c.clip();
  for(let k=0;k<plot.fields.length;k++) {
    c.strokeStyle=palette[k];c.fillStyle=palette[k];
    let segment=[];
    const paint=()=>{
      if(!segment.length)return;
      c.globalAlpha=.12;c.beginPath();
      segment.forEach((r,i)=>{const px=x(center(r)),py=y(field(r,plot.invert?.[k]?'min':'max',plot,k)/divisor);if(i)c.lineTo(px,py);else c.moveTo(px,py);});
      for(let i=segment.length-1;i>=0;i--){const r=segment[i];c.lineTo(x(center(r)),y(field(r,plot.invert?.[k]?'max':'min',plot,k)/divisor));}
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
  if(!hasValue){c.fillStyle=style.getPropertyValue('--muted');c.textAlign='center';c.fillText(plot.id==='temp'?'Sensor unavailable':plot.id==='frequency'?'CPU frequency unavailable':plot.id==='memory'&&showSwap&&!state.swap?'No swap configured':'Waiting for samples',(left+right)/2,(top+bottom)/2);}
  plot.geometry={w,h,left,right,top,bottom,width,start,end,x,y,center,divisor};
  plot.tipKey='';
  ensureMarkers(plot);
  canvas.setAttribute('aria-label',plot.id+' history: average lines and minimum/maximum bands. Latest: '+plot.fields.map((f,k)=>plot.labels[k]+' '+formatValue(plot,state.latest[f])).join(', '));
}
function queueDraw(){if(!frame)frame=requestAnimationFrame(()=>{frame=0;redraw();});}
async function download(name) {
  const response=await fetch(name,{cache:'no-cache',signal:AbortSignal.timeout(10000)});
  if(!response.ok)throw Error(name+': HTTP '+response.status);
  return response.arrayBuffer();
}
async function refresh() {
  clearTimeout(pollTimer);if(document.hidden||busy)return;busy=true;
  const index=Number(range.value);
  try {
    skipped=0;const next=decodeLive(await download(liveFile));next.index=index;
    const active=next.active[index],key=index+':'+next.points+':'+next.interval+':'+next.metadataHash+':'+next.frequencyBits+':'+next.committed[index];
    if(!historyCache||historyCache.key!==key){const data=decodeHistory(await download(files[index]),index,next);historyCache={key,data,skipped};}
    else skipped=historyCache.skipped;
    if(index!==Number(range.value))return;
    const cutoff=next.latestTime-spans[index]*1000,width=next.widths[index]*1000;
    const merged=new Map(historyCache.data.map(r=>[r.start,r]));
    if(index===0)for(const row of next.pending)merged.set(row.start,row);
    if(active)merged.set(active.start,active);
    visible=[...merged.values()].filter(r=>r.start+width>cutoff&&r.start<= (active?.start??next.latestTime)).sort((a,b)=>a.start-b.start).slice(-next.points);
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
$('network-toggle').addEventListener('click',()=>{if(!state||state.interfaces<2)return;selectedInterface=state.interfaceNames[(interfaceIndex+1)%state.interfaces];clearInspection();configurePlots();summaries();queueDraw();});
range.addEventListener('change',()=>{clearInspection();refresh();});
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
  return new Date(row.start).toLocaleString()+' · '+plot.fields.map((f,k)=>plot.labels[k]+' average '+fmt(field(row,'mean',plot,k))+', range '+fmt(field(row,plot.invert?.[k]?'max':'min',plot,k))+'–'+fmt(field(row,plot.invert?.[k]?'min':'max',plot,k))+(plot.invert?.[k]?'':', peak '+new Date(row.peak[f]).toLocaleTimeString())).join(' · ');
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
