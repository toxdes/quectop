export const schemaVersion=1,diskSchemaVersion=1;
export const spans=[3600,86400,604800,2592000,7776000];
export const files=['1h','1d','7d','30d','90d'].map(name=>name+'.v'+schemaVersion+'.bin');
export const liveFile='live.v'+schemaVersion+'.bin',disksFile='disks.v'+diskSchemaVersion+'.bin';
export let skipped=0;

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
/* Metric order matches the probe; metrics from memory to network keep bucket minimums. */
function layout(cores,frequencies,frequencyBits,interfaces) {
  const c=cores,temp=c+9,frequency=c+10,network=frequency+frequencies;
  const bits=Array(temp).fill(10).concat(12,Array(frequencies).fill(frequencyBits),Array(interfaces*2).fill(14));
  return {cores,bits,valid:new Uint8Array(bits.length),metrics:bits.length,valueBits:10*temp+12+frequencies*frequencyBits+28*interfaces,
    lowBits:32+frequencies*frequencyBits,overall:c,user:c+1,system:c+2,iowait:c+3,pressure:c+4,memory:c+7,swap:c+8,temp,frequency,network};
}
function unpackValues(read,valid,format,from=0,to=format.metrics) {
  const bits=format.bits,values=new Array(bits.length);
  for(let k=from;k<to;k++){
    const code=read(bits[k]);
    if(!valid[k])values[k]=null;
    else if(k<format.temp){if(code>1000)throw Error('Invalid percentage');values[k]=code/10;}
    else if(k===format.temp)values[k]=(code-2048)/10;
    else if(k<format.network)values[k]=code;
    else{
      const mantissa=code&1023,decimals=(code>>10)&3,unit=(code>>12)&3;
      if(mantissa>999||decimals>2)throw Error('Invalid network rate');
      values[k]=mantissa*1000**unit/10**decimals;
    }
  }
  return values;
}
/* Validity bits go to the layout's scratch array, reused for every record. */
function readValid(read,format) {
  let any=0;
  for(let k=0;k<format.metrics;k++)any|=format.valid[k]=read(1);
  return any;
}
function recordBytes(width,format){return Math.ceil((32+format.metrics+2*format.valueBits+format.lowBits+format.metrics*Math.max(1,Math.ceil(Math.log2(width))))/8)+4;}
function decodeBucket(d,at,width,bytes,format) {
  if(!d.getUint32(at,true))return null;
  if(!validHash(d,at,bytes)){skipped++;return null;}
  const read=reader(d,at),start=read(32)*1000,valid=format.valid;
  if(!readValid(read,format))return null;
  if(start/1000%width)throw Error('Misaligned bucket');
  const mean=unpackValues(read,valid,format),min=unpackValues(read,valid,format,format.memory,format.network),max=unpackValues(read,valid,format);
  const offsetBits=Math.max(1,Math.ceil(Math.log2(width)));
  const peak=new Array(format.metrics);
  for(let k=0;k<format.metrics;k++)peak[k]=start+read(offsetBits)*1000;
  for(let k=0;k<format.metrics;k++)if(mean[k]!=null&&(min[k]!=null&&mean[k]<min[k]||mean[k]>max[k]||peak[k]>=start+width*1000))throw Error('Invalid bucket values');
  return {start,mean,min,max,peak};
}
const serviceDetailFields = ['uss', 'anon', 'file', 'shmem'];

function decodeServices(d, at, count, decoder) {
  const services = [], end = d.byteLength - 4;
  const varint = () => {
    let value = 0;
    for (let i = 0; i < 10 && at < end; i++) {
      const byte = d.getUint8(at++);
      if (i === 9 && byte > 1) break;
      value += (byte & 127) * 2 ** (7 * i);
      if (!(byte & 128)) {
        if (i && byte === 0) break;
        return value;
      }
    }
    throw Error('Invalid service counter');
  };

  for (let i = 0; i < count; i++) {
    if (at + 2 > end) throw Error('Incomplete service entry');
    const length = d.getUint8(at++), flags = d.getUint8(at++);
    const status = flags & 7, hasUptime = Boolean(flags & 16);
    if (!length || at + length > end ||
        hasUptime && status !== 2 && status !== 6 ||
        (flags & 128) && !hasUptime) throw Error('Invalid service entry');
    const name = decoder.decode(new Uint8Array(d.buffer, d.byteOffset + at, length));
    at += length;
    const row = {
      name, status, memory: flags & 8 ? varint() : null,
      uptime: hasUptime ? varint() : null,
      openrc: Boolean(flags & 64), processAge: Boolean(flags & 128),
      rss: null, pss: null, uss: null, anon: null, file: null, shmem: null
    };
    if (flags & 32) {
      if (at >= end) throw Error('Incomplete service memory');
      const mask = d.getUint8(at++);
      if (!mask || mask > 63 || (mask & 60) && !(mask & 2))
        throw Error('Invalid service memory mask');
      if (mask & 1) row.rss = varint() * 1024;
      if (mask & 2) row.pss = varint() * 1024;
      const fields = serviceDetailFields.filter((_, i) => mask & (4 << i));
      const bytes = Math.ceil(fields.length * 17 / 8);
      if (at + bytes > end) throw Error('Incomplete service memory');
      const packed = reader(d, at);
      for (const field of fields) {
        const code = packed(17), mantissa = code >>> 3;
        const decimals = (code >>> 2) & 1, unit = code & 3;
        if (mantissa > 9999) throw Error('Invalid service memory value');
        row[field] = mantissa / (decimals ? 10 : 1) * 2 ** (10 * (unit + 1));
      }
      at += bytes;
    }
    services.push(row);
  }
  if (at !== end) throw Error('Invalid live snapshot length');
  return services;
}

export function decodeLive(buffer) {
  skipped=0;
  const d=new DataView(buffer);
  if(d.byteLength<148||!magic(d,'QUECTOPL')||d.getUint32(8,true)!==schemaVersion||!validHash(d,0,d.byteLength))throw Error('Invalid '+liveFile+' (expected schema v'+schemaVersion+')');
  const points=d.getUint32(12,true),interval=d.getUint32(16,true),publish=d.getUint32(20,true);
  const cores=d.getUint32(52,true),idBits=d.getUint32(56,true),metrics=d.getUint32(60,true);
  const interfaces=d.getUint32(108,true),frequencies=d.getUint32(120,true),frequencyBits=d.getUint32(124,true),metadataBytes=d.getUint32(128,true);
  if(points<16||points>10000||interval<100||!publish||!cores||cores>1048576||interfaces>65535||frequencies>65535||frequencies>cores||frequencyBits<1||frequencyBits>16||metrics!==cores+10+frequencies+2*interfaces||idBits<1||idBits>32)throw Error('Invalid live configuration');
  const format=layout(cores,frequencies,frequencyBits,interfaces),idBytes=Math.ceil((cores*idBits+frequencies*(idBits+1))/8),latestBytes=Math.ceil((format.metrics+format.valueBits)/8);
  if(metadataBytes<idBytes||metadataBytes>idBytes+interfaces*16)throw Error('Invalid device metadata');
  const widths=spans.map((span,i)=>{const width=d.getUint32(64+i*4,true),expected=Math.max(Math.ceil(span/(points-1)),i===0?Math.ceil(interval/1000):1);if(width!==expected)throw Error('Invalid bucket width');return width;});
  const sizes=widths.map(width=>recordBytes(width,format)),pendingCount=d.getUint32(104,true),serviceCount=d.getUint32(132,true);
  const capacity=Math.max(1,Math.floor(1024/sizes[0]));
  if(serviceCount>65535||pendingCount>capacity||d.byteLength<149+metadataBytes+latestBytes+sizes.reduce((a,b)=>a+b,0)+pendingCount*sizes[0])throw Error('Incomplete '+liveFile);
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
  const read=reader(d,at);readValid(read,format);const latest=unpackValues(read,format.valid,format);
  at+=latestBytes;
  const active=widths.map((width,i)=>{const row=decodeBucket(d,at,width,sizes[i],format);at+=sizes[i];if(row)row.current=true;return row;});
  const pending=[];
  for(let i=0;i<pendingCount;i++){const row=decodeBucket(d,at,widths[0],sizes[0],format);at+=sizes[0];if(row)pending.push(row);}
  const hostBytes=d.getUint8(at++);
  if(hostBytes>64||at+hostBytes>d.byteLength-4)throw Error('Invalid host name');
  const hostname=decoder.decode(new Uint8Array(buffer,at,hostBytes));at+=hostBytes;
  const services=decodeServices(d,at,serviceCount,decoder);
  return {points,interval,publish,cores,cpuIds,idBits,idBytes,metadataBytes,metadata,metadataHash,interfaces,interfaceNames,frequencies,frequencyBits,frequencyIds,frequencyHardware,format,widths,sizes,active,latest,pending,services,hostname,
    committed:spans.map((_,i)=>d.getUint32(84+i*4,true)),
    generated:Number(d.getBigUint64(24,true)),uptime:Number(d.getBigUint64(32,true)),memory:Number(d.getBigUint64(40,true)),latestTime:d.getUint32(48,true)*1000,
    swap:Number(d.getBigUint64(112,true)),disksGenerated:Number(d.getBigUint64(136,true))};
}
export function decodeHistory(buffer,index,next) {
  const d=new DataView(buffer),bytes=next.sizes[index],width=next.widths[index],header=64+next.metadataBytes;
  if(d.byteLength!==header+(next.points-1)*bytes||!magic(d,'QUECTOPH')||d.getUint32(8,true)!==schemaVersion||d.getUint32(12,true)!==next.points||d.getUint32(16,true)!==spans[index]||d.getUint32(20,true)!==width||d.getUint32(24,true)!==next.interval||d.getUint32(28,true)!==bytes||d.getUint32(32,true)!==index||d.getUint32(36,true)!==next.cores||d.getUint32(40,true)!==next.format.metrics||d.getUint32(44,true)!==header||d.getUint32(48,true)!==next.idBits||d.getUint32(52,true)!==next.interfaces||d.getUint16(56,true)!==next.frequencies||d.getUint8(58)!==next.frequencyBits||d.getUint8(59)!==0)throw Error('History configuration mismatch');
  const raw=new Uint8Array(buffer);
  if(checksum(raw,64,next.metadataBytes,checksum(raw,0,60))!==d.getUint32(60,true))throw Error('Invalid history header');
  if(next.metadata.some((byte,i)=>byte!==raw[64+i]))throw Error('History device metadata mismatch');
  const rows=[];
  for(let at=header;at<d.byteLength;at+=bytes){const row=decodeBucket(d,at,width,bytes,next.format);if(row)rows.push(row);}
  return rows.sort((a,b)=>a.start-b.start);
}
export function decodeDisks(buffer) {
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
