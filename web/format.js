/* Text formatting shared by every tile. */
export const percent=n=>n==null?'—':n.toFixed(1)+'%';
export const age=ms=>ms<60000?Math.floor(ms/1000)+'s':ms<3600000?Math.floor(ms/60000)+'m':ms<86400000?Math.floor(ms/3600000)+'h':Math.floor(ms/86400000)+'d';
/* Four-significant-digit mantissa with K/M/G/T/P letters; binary base for
 * capacities, decimal base for rates. A value just below the next unit never
 * rounds up to it. */
export const byteUnits=['B','K','M','G','T','P'];
export function byteScale(n,base) {
  let unit=0;const limit=base*.99995;
  while(n>=limit&&unit<byteUnits.length-1){n/=base;unit++;}
  return[n,byteUnits[unit]];
}
export function mantissa(value) {
  const scaled=Math.round(value*10)/10;
  return (value>0&&!scaled?'0.1':scaled.toFixed(1));
}
function byteText(n,base) {
  if(n==null)return '—';
  const[value,unit]=byteScale(n,base);
  return mantissa(value)+unit;
}
export const capacity=n=>byteText(n,1024);
export const shortSpeed=n=>byteText(n,1000);
export const speed=n=>n==null?'—':byteText(n,1000)+'/s';
export function capacityPair(used,total) {
  const left=capacity(used),right=capacity(total),unit=right.at(-1);
  return (left.endsWith(unit)?left.slice(0,-1):left)+'/'+right;
}
export function serviceAge(seconds) {
  if(seconds>=86400)return Math.floor(seconds/86400)+'d '+Math.floor(seconds%86400/3600)+'h';
  if(seconds>=3600)return Math.floor(seconds/3600)+'h '+Math.floor(seconds%3600/60)+'m';
  if(seconds>=60)return Math.floor(seconds/60)+'m '+seconds%60+'s';
  return seconds+'s';
}
export const colorNames=['--teal','--violet','--third','--fourth','--core5','--core6','--core7','--core8'];
export const seriesColor=k=>k<colorNames.length?'var('+colorNames[k]+')':'hsl('+((k*137.508)%360).toFixed(1)+' 55% var(--series-lightness))';
