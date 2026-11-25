'use strict';
const fs=require('fs'),path=process.argv.at(-1);
const NativeTextDecoder=globalThis.TextDecoder;
let decoderCount=0;
globalThis.TextDecoder=class extends NativeTextDecoder{constructor(...args){super(...args);decoderCount++;}};
const {decodeLive:decode}=require('../web/codec.js');
const bytes=fs.readFileSync(path+'/live.v1.bin'),entry=fs.readFileSync(path+'/service-entry.bin');
const array=data=>data.buffer.slice(data.byteOffset,data.byteOffset+data.byteLength);
const assert=(value,message)=>{if(!value)throw Error(message);};
const row=decode(array(bytes)).services[0];
assert(decoderCount===1,'one decoder shared by interface and service names');
assert(row.openrc&&row.processAge&&row.status===2,'backend and uptime origin');
assert(row.memory===987654&&row.rss===1200*1024&&row.pss===900*1024,'lossless memory totals');
for(const [field,expected] of [['uss',600],['anon',450],['file',300],['shmem',150]])assert(row[field]===expected*1024,'lossless component values: '+field);
const entryAt=bytes.length-4-entry.length;
let maskAt=entryAt+2+entry[0];
for(let field=0;field<2;field++)while(bytes[maskAt++]&128){}
function corrupt(offset,value){
  const changed=Buffer.from(bytes);changed[offset]=value;
  let hash=2166136261;for(const byte of changed.subarray(0,-4))hash=Math.imul(hash^byte,16777619)>>>0;
  changed.writeUInt32LE(hash,changed.length-4);
  let failed=false;try{decode(array(changed));}catch{failed=true;}
  assert(failed,'reject malformed memory extension');
}
corrupt(maskAt,64);
corrupt(maskAt,4);
corrupt(entryAt+1,128);
console.log('service wire: totals, components, backend flags and malformed extensions passed');
