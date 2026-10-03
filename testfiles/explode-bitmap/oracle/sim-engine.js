

// Pure alpha-channel engine, shared verbatim by the worker and Node tests.
function explode(alpha,w,h,o){
 const started=performance.now(),times={},warnings=[],n=w*h;let tick=started;
 const stage=name=>{let t=performance.now();times[name]=t-tick;tick=t};
 if(w>32767||h>32767)throw Error('La imagen supera el límite de 32.767 px por eje.');
 // The plan supplies no numeric RSS/B budget; simulator reserves conservative caps.
 const cap=512*1024*1024,Bcap=64e6;
 if(n*25>cap)throw Error('Presupuesto local estimado de 512 MiB excedido antes del etiquetado.');
 const labels=new Int32Array(n),queue=new Int32Array(n),regions=[null];let faint=0,core=0;
 for(let i=0;i<n;i++){if(alpha[i]>o.T){labels[i]=-1;core++}else if(alpha[i])faint++}
 stage('Máscara');
 if(!core){stage('Etiquetado');times.Total=performance.now()-started;return {labels,pieces:[],times,warnings:['Sin núcleo: imagen sin cambios; los píxeles tenues no se eliminan.'],counts:{islands:0,gap:0,enclosure:0,dots:0,removed:0,faint,small:0},memory:n*9,B:0,noOp:true}}
 let k=0;
 for(let i=0;i<n;i++)if(labels[i]===-1){let id=++k,head=0,tail=1;queue[0]=i;labels[i]=id;let area=0;while(head<tail){let p=queue[head++],x=p%w,y=(p/w)|0;area++;for(let dy=-1;dy<=1;dy++)for(let dx=-1;dx<=1;dx++){if(!dx&&!dy||o.conn===4&&dx&&dy)continue;let xx=x+dx,yy=y+dy;if(xx<0||xx>=w||yy<0||yy>=h)continue;let q=yy*w+xx;if(labels[q]===-1){labels[q]=id;queue[tail++]=q}}}regions.push({area,first:i})}
 stage('Etiquetado');
 if(n*13+k*256>cap)throw Error('Las islas y el grafo de regiones superan el presupuesto local.');
 const parent=new Int32Array(k+1);for(let a=1;a<=k;a++)parent[a]=a;
 function root(a){while(parent[a]!==a){parent[a]=parent[parent[a]];a=parent[a]}return a}
 function join(a,b){a=root(a);b=root(b);if(a===b)return false;if(a>b)[a,b]=[b,a];parent[b]=a;return true}
 let grouped=0,merged=0,dots=0;
 if(o.enclosure&&o.conn===8){
 // Alternating foreground/background region graph; exterior is background node zero.
 const bg=new Int32Array(n),graph=new Map();let bk=0;
 function edge(a,b){if(!graph.has(a))graph.set(a,new Set());if(!graph.has(b))graph.set(b,new Set());graph.get(a).add(b);graph.get(b).add(a)}
 for(let i=0;i<n;i++)if(labels[i]===0&&!bg[i]){let id=++bk,head=0,tail=1,exterior=false,adj=new Set();bg[i]=id;queue[0]=i;while(head<tail){let p=queue[head++],x=p%w,y=(p/w)|0;if(!x||!y||x===w-1||y===h-1)exterior=true;for(let direction=0;direction<4;direction++){let q=direction===0?(x?p-1:-1):direction===1?(x<w-1?p+1:-1):direction===2?(y?p-w:-1):(y<h-1?p+w:-1);if(q>=0){if(labels[q]>0)adj.add(labels[q]);else if(!bg[q]){bg[q]=id;queue[tail++]=q}}}}let node=exterior?0:-id;for(let a of adj)edge(node,a)}
 for(let x=0;x<w;x++){if(labels[x]>0)edge(0,labels[x]);if(labels[n-w+x]>0)edge(0,labels[n-w+x])}for(let y=0;y<h;y++){if(labels[y*w]>0)edge(0,labels[y*w]);if(labels[y*w+w-1]>0)edge(0,labels[y*w+w-1])}
 const depth=new Map([[0,0]]),prev=new Map(),todo=[0];for(let at=0;at<todo.length;at++){let a=todo[at];for(let b of graph.get(a)||[])if(!depth.has(b)){depth.set(b,depth.get(a)+1);prev.set(b,a);todo.push(b)}}
 for(let a=1;a<=k;a++)if(depth.get(a)>1){let container=prev.get(prev.get(a));if(container>0&&join(a,container))grouped++}
 }else if(o.enclosure)warnings.push('Conectividad 4 experimental: contención desactivada.');
 stage('Contención');
 // Boundary pixels in an in-place balanced kd tree. Distances are between closed pixel cells.
 let count=0;for(let p=0;p<n;p++)if(labels[p]>0){let x=p%w,y=(p/w)|0;if(!x||!y||x===w-1||y===h-1||labels[p-1]!==labels[p]||labels[p+1]!==labels[p]||labels[p-w]!==labels[p]||labels[p+w]!==labels[p])queue[count++]=p}
 const boundary=queue.subarray(0,count),coord=(p,axis)=>axis?Math.floor(p/w):p%w;
 function select(lo,hi,mid,axis){while(lo<hi){let pivot=coord(boundary[(lo+hi)>>1],axis),i=lo,j=hi;while(i<=j){while(coord(boundary[i],axis)<pivot)i++;while(coord(boundary[j],axis)>pivot)j--;if(i<=j){let t=boundary[i];boundary[i++]=boundary[j];boundary[j--]=t}}if(mid<=j)hi=j;else if(mid>=i)lo=i;else break}}
 function build(lo,hi,axis){if(lo>hi)return;let m=(lo+hi)>>1;select(lo,hi,m,axis);build(lo,m-1,1-axis);build(m+1,hi,1-axis)}
 if(faint||o.D>0||o.dots)build(0,count-1,0);
 function search(p,limit,excluded=0,merge=false,original=false){let x=p%w,y=(p/w)|0,best=limit,bestId=0;function walk(lo,hi,axis){if(lo>hi)return;let m=(lo+hi)>>1,q=boundary[m],id=labels[q],dx=Math.max(0,Math.abs(x-q%w)-1),dy=Math.max(0,Math.abs(y-Math.floor(q/w))-1),d=dx*dx+dy*dy;if((!excluded||(original?id!==excluded:root(id)!==root(excluded)))&&d<=best){if(merge){if(join(excluded,id))merged++}else if(d<best||!bestId||id<bestId){best=d;bestId=id}}let delta=(axis?y:x)-coord(q,axis),near=delta<=0;walk(near?lo:m+1,near?m-1:hi,1-axis);if(Math.max(0,Math.abs(delta)-1)**2<=best)walk(near?m+1:lo,near?hi:m-1,1-axis)}walk(0,count-1,0);return {id:bestId,d:best}}
 if(o.D>0)for(let p of boundary)search(p,o.D*o.D,labels[p],true);
 stage('Fusión por distancia');
 if(o.dots&&o.S>0){
 const areas=new Float64Array(k+1);for(let a=1;a<=k;a++)areas[root(a)]+=regions[a].area;
 const nearest=new Map();for(let a=1;a<=k;a++)if(parent[a]===a&&areas[a]<=o.S)nearest.set(a,{id:0,d:o.reach**2});
 for(let p of boundary){let a=root(labels[p]),best=nearest.get(a);if(!best)continue;let v=search(p,best.d,a);if(v.id&&(v.d<best.d||!best.id||v.id<best.id))nearest.set(a,v)}
 for(let [a,b] of nearest)if(b.id&&join(a,b.id))dots++;
 }
 let removed=0,distant=0;for(let p=0;p<n;p++)if(alpha[p]>0&&alpha[p]<=o.T){let v=search(p,Infinity);if(v.d>o.reach**2){distant++;if(o.remove){removed++;continue}}labels[p]=v.id}
 if(distant&&!o.remove)warnings.push(`${distant.toLocaleString('es')} píxeles tenues distantes conservados; pueden ampliar las cajas.`);
 stage('Puntos y píxeles tenues');
 const boxes=new Map();for(let p=0;p<n;p++)if(labels[p]>0){let id=labels[p]=root(labels[p]),b=boxes.get(id),x=p%w,y=(p/w)|0;if(!b){b={id,x,y,x2:x,y2:y,area:0};boxes.set(id,b)}b.x=Math.min(b.x,x);b.y=Math.min(b.y,y);b.x2=Math.max(b.x2,x);b.y2=Math.max(b.y2,y);b.area++}
 let small=0;const rejected=new Set();let pieces=[];for(let b of boxes.values())if(b.area<o.min){small++;rejected.add(b.id)}else{b.cx=b.x-o.gutter;b.cy=b.y-o.gutter;b.width=b.x2-b.x+1+2*o.gutter;b.height=b.y2-b.y+1+2*o.gutter;pieces.push(b)}
 if(small)for(let p=0;p<n;p++)if(rejected.has(labels[p]))labels[p]=0;
 pieces.sort((a,b)=>a.y-b.y||a.x-b.x||a.id-b.id);let B=pieces.reduce((s,b)=>s+b.width*b.height,0);
 if(pieces.some(b=>b.width>32767||b.height>32767))throw Error('Un recorte con margen supera 32.767 px.');
 if(pieces.length>20000)throw Error('Más de 20.000 piezas: resultado rechazado.');if(pieces.length>1000)warnings.push('Más de 1.000 piezas: el plan exige confirmación antes de aplicar.');if(B>Bcap)throw Error('Los recortes superan el presupuesto local de 64 MP.');
 const memory=n*17+B*4+(k+1)*128;if(memory>cap)throw Error('La estimación con recortes supera 512 MiB.');
 stage('Recortes y filtro');times.Total=performance.now()-started;
 return {labels,pieces,times,warnings,counts:{islands:k,gap:merged,enclosure:grouped,dots,removed,faint,small},memory,B,noOp:pieces.length===1};
}
// Deterministic procedural samples; identical raster generators are used in Node.
function sampleRaster(kind){const w=320,h=220,data=new Uint8ClampedArray(w*h*4);function put(x,y,a,r,g,b){let p=(y*w+x)*4;data[p]=r;data[p+1]=g;data[p+2]=b;data[p+3]=a}for(let y=0;y<h;y++)for(let x=0;x<w;x++){if(kind==='stickers'){for(let [cx,cy,r,c] of [[60,55,27,[239,103,90]],[175,55,32,[80,180,240]],[75,160,30,[248,198,74]],[240,150,37,[166,120,240]]]){let d=Math.hypot(x-cx,y-cy);if(d<r+0.5)put(x,y,Math.round(255*Math.min(1,r+0.5-d)),...c)}}else if(kind==='ring'){let d=Math.hypot(x-160,y-110);if(d>=55&&d<=78)put(x,y,255,80,190,220);if(d<20)put(x,y,255,245,162,65)}else if(kind==='near'){if(y>=65&&y<155&&((x>=45&&x<105)||(x>=110&&x<170)||(x>=250&&x<285)))put(x,y,255,130,185,240)}else{let d=Math.hypot(x-90,y-110);if(d<44)put(x,y,Math.round(255*Math.min(1,(44-d)/5)),240,110,160);else if(d<51)put(x,y,4,240,110,160);let e=Math.hypot(x-215,y-110);if(e<35)put(x,y,Math.round(255*Math.min(1,(35-e)/4)),100,210,150);if(x>=295&&x<299&&y>=15&&y<19)put(x,y,3,90,100,220)}}return {w,h,data}}
/* AlphaKiller imageProcessing.js
MIT License

Copyright (c) 2026 Abraham Saenz

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

function alphaThreshold(imageData,{threshold,softness}){
 const low=threshold-softness,high=threshold+softness,table=new Uint8Array(256);
 for(let a=0;a<256;a++){let t=(a-low)/Math.max(1,2*softness);table[a]=softness===0?(a>=threshold?255:0):a<=low?0:a>=high?255:Math.round(t*t*(3-2*t)*255)}
 for(let i=3;i<imageData.data.length;i+=4)imageData.data[i]=table[imageData.data[i]];
}
function proxyRaster(r,maxSide=768){
 const scale=Math.min(1,maxSide/Math.max(r.w,r.h));if(scale===1)return {...r,scale:1};
 const w=Math.max(1,Math.round(r.w*scale)),h=Math.max(1,Math.round(r.h*scale)),data=new Uint8ClampedArray(w*h*4),src=new Uint32Array(r.data.buffer,r.data.byteOffset,r.w*r.h),dst=new Uint32Array(data.buffer);
 for(let y=0;y<h;y++){let row=Math.min(r.h-1,Math.floor((y+.5)*r.h/h))*r.w;for(let x=0;x<w;x++)dst[y*w+x]=src[row+Math.min(r.w-1,Math.floor((x+.5)*r.w/w))]}
 return {w,h,data,scale};
}
function preview(r,T=128,S=40,scale=1){
 const start=performance.now(),data=new Uint8ClampedArray(r.data);alphaThreshold({data},{threshold:T,softness:S});
 const alpha=new Uint8Array(r.w*r.h);for(let p=0;p<alpha.length;p++)alpha[p]=data[p*4+3];
 const px=300/25.4*scale;
 const partition=explode(alpha,r.w,r.h,{T:0,D:0,reach:px,enclosure:true,remove:false,dots:true,S:2*px*px,min:0,gutter:1,conn:8});
 return {w:r.w,h:r.h,data,partition,T,S,scale,ms:performance.now()-start};
}
function bakePieces(result){
 const {data,w,h,partition}=result,src=new Uint32Array(data.buffer,data.byteOffset,w*h);
 return partition.pieces.map(b=>{const pixels=new Uint8ClampedArray(b.width*b.height*4),dst=new Uint32Array(pixels.buffer);
 for(let y=b.y;y<=b.y2;y++)for(let x=b.x;x<=b.x2;x++){let p=y*w+x;if(partition.labels[p]===b.id)dst[(y-b.cy)*b.width+x-b.cx]=src[p]}
 return {type:'bitmap',id:b.id,x:b.cx,y:b.cy,width:b.width,height:b.height,data:pixels};
 });
}
// One transaction: source and alpha parameters survive undo, plain bitmaps survive redo.
class BitmapFlow{
 constructor(source,T=128,S=40){this.source=source;this.T=T;this.S=S;this.phase='adjust';this.pieces=[];this.transaction=null}
 setAlpha(T,S){if(this.phase!=='adjust')return;this.T=T;this.S=S;this.transaction=null}
 explode(result=preview(this.source,this.T,this.S)){
 if(this.phase!=='adjust')return false;
 if(result.scale!==1||result.T!==this.T||result.S!==this.S||result.w!==this.source.w||result.h!==this.source.h)throw Error('La vista exacta todavía no está lista.');
 this.transaction={source:this.source,T:this.T,S:this.S,pieces:bakePieces(result),result};this.pieces=this.transaction.pieces;this.phase='pieces';return true;
 }
 undo(){if(this.phase!=='pieces'||!this.transaction)return false;Object.assign(this,{source:this.transaction.source,T:this.transaction.T,S:this.transaction.S,phase:'adjust',pieces:[]});return true}
 redo(){if(this.phase!=='adjust'||!this.transaction)return false;this.pieces=this.transaction.pieces;this.phase='pieces';return true}
}
function inspectionPixels(data,alphaOnly,highlight,lo=1,hi=200){
 const out=new Uint8ClampedArray(data);let count=0;
 if(alphaOnly||highlight)for(let i=0;i<out.length;i+=4){const a=data[i+3];if(alphaOnly){out[i]=out[i+1]=out[i+2]=a;out[i+3]=255}if(highlight&&a>=lo&&a<=hi){out[i]=255;out[i+1]=0;out[i+2]=255;out[i+3]=255;count++}}
 return {data:out,count};
}

