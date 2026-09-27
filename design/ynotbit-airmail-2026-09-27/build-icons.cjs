// Reproducible vector artwork and platform exports. Requires Node.js and sharp.
const fs = require('node:fs');
const path = require('node:path');
const cp = require('node:child_process');
const sharp = require('sharp');
const secure = process.env.ICON_FAMILY === 'protected';
const root = secure ? path.join(__dirname,'protected') : __dirname;
const sizes = [16,20,24,32,40,48,64,96,128,256,512,1024];
const variants = secure ? ['signal-shield','signal-clasp','signal-seal'] : ['paper','night','signal'];
const manifest = [];
const write = (p,d) => { fs.mkdirSync(path.dirname(p),{recursive:true});fs.writeFileSync(p,d); };
function artwork(v,s,role='application',platform='universal',logical=s) {
  const small=logical<=32, tiny=logical<=20, mac=platform==='macos';
  const u=s/100, q=n=> small?Math.round(n*u*logical/s)*s/logical:n*u;
  const inset=mac?10:6;
  let x=q(inset), y=q(inset), w=s-2*x, h=w;
  if(role==='window') {x=q(5);w=s-2*x;h=q(68);y=Math.round((s-h)/2);}
  const dark=v==='night', simple=v.startsWith('signal');
  let body='';
  const rect=(a,b,c,d,r,fill,extra='')=>`<rect x="${a}" y="${b}" width="${c}" height="${d}" rx="${r}" fill="${fill}" ${extra}/>`;
  const line=(d,color,width)=>`<path d="${d}" fill="none" stroke="${color}" stroke-width="${width}" stroke-linecap="round" stroke-linejoin="round"/>`;
  if(dark&&role==='application') {
    body+=rect(x,y+q(1),w,h,q(21),'#0B192B');
    body+=rect(x,y,w,h,q(21),small?'#183650':'url(#navy)');
    if(!small) body+=rect(x+q(.6),y+q(.6),w-q(1.2),h-q(1.2),q(20.5),'none',`stroke="#8AA7C1" stroke-opacity=".25" stroke-width="${q(.8)}"`);
    x+=q(9);w-=q(18);h=q(52);y=q(27);
  }
  const radius=q(role==='window'?8:(dark?7:19));
  if(!small) body+=rect(x,y+q(1.8),w,h,radius,dark?'#091827':'#BDB4A2',`opacity="${dark?.45:.28}"`);
  const base=dark&&role==='window'?'#183650':small?'#FFF9EC':'url(#paper)';
  body+=rect(x,y,w,h,radius,base);
  if(!small) body+=rect(x+q(.5),y+q(.5),w-q(1),h-q(1),radius,'none',`stroke="${dark&&role==='window'?'#536D85':'#B9B7AD'}" stroke-width="${q(.65)}"`);
  // Edge stripes are real clipped geometry, never a scaled raster pattern.
  let stripes='';
  const red='#DB5149', blue='#2871B8';
  const bw=Math.max(s/logical,q(simple?8:small?9:7));
  if(simple || (dark&&role==='window')) {
    const sh=q(tiny?15:19), sl=q(6);
    for(let i=0;i<2;i++) {
      const yy=y+h*(tiny?.37:.34)+i*h*.32;
      stripes+=`<path d="M${x} ${yy-sl}l${bw} ${sl}v${sh}l${-bw} ${-sl}z" fill="${i?blue:red}"/>`;
      stripes+=`<path d="M${x+w} ${yy-sl}l${-bw} ${sl}v${sh}l${bw} ${-sl}z" fill="${i?red:blue}"/>`;
    }
  } else {
    const count=tiny?4:small?6:10;
    const step=w/count;
    for(let i=-1;i<=count;i++) {
      const xx=x+i*step, b=step*.62;
      stripes+=`<path d="M${xx} ${y}h${b}l${-bw} ${bw}h${-b}z M${xx} ${y+h-bw}h${b}l${-bw} ${bw}h${-b}z" fill="${i%2?blue:red}"/>`;
    }
    for(let i=0;i<(tiny?2:3);i++) {
      const yy=y+bw+i*(h-bw*2)/(tiny?2:3), sh=(h-bw*2)/(tiny?2:3)*.55;
      stripes+=`<path d="M${x} ${yy}l${bw} ${bw}v${sh}l${-bw} ${-bw}z" fill="${i%2?red:blue}"/>`;
      stripes+=`<path d="M${x+w} ${yy}l${-bw} ${bw}v${sh}l${bw} ${-bw}z" fill="${i%2?blue:red}"/>`;
    }
  }
  body+=`<g clip-path="url(#env)">${stripes}</g>`;
  const seam=small?(simple||dark&&role==='window'?Math.max(s/logical,q(5)):s/logical):q(.9);
  const cx=s/2-(small&&Math.round(seam*logical/s)%2?s/logical/2:0);
  const junction=small?Math.round((y+h*.54)*logical/s)*s/logical:y+h*.54;
  const fy=small?Math.round((y+h*(simple?.26:.22))*logical/s)*s/logical:y+h*(simple?.26:.22), fx=x+(simple?0:bw*.7);
  if(simple || (dark&&role==='window')) {
    const color=dark?'#FFF8EA':'#19334B';
    body+=`<g clip-path="url(#env)">`+line(`M${x} ${fy}L${cx} ${junction}L${x+w} ${fy}`,color,small?seam:q(4.6))+line(`M${cx} ${junction}V${small?q((y+h*.79)/u):y+h*.79}`,color,small?seam:q(4.6))+'</g>';
  } else {
    // Large sizes keep a soft folded flap. Small sizes use one crisp seam.
    if(!small) {
      body+=line(`M${x+bw} ${y+h-bw}L${cx} ${junction}L${x+w-bw} ${y+h-bw}`,'#D7CFC0',q(.8));
      body+=`<path d="M${fx} ${fy+q(1)}L${cx} ${junction+q(2.6)}L${x+w-(fx-x)} ${fy+q(1)}L${cx} ${junction+q(4)}Z" fill="#877D6C" opacity=".2"/>`;
      body+=`<path d="M${fx} ${y+bw}V${fy}L${cx-q(2)} ${junction-q(.7)}Q${cx} ${junction+q(.7)} ${cx+q(2)} ${junction-q(.7)}L${x+w-(fx-x)} ${fy}V${y+bw}Z" fill="url(#flap)"/>`;
    }
    body+=line(`M${fx} ${fy}L${cx} ${junction}L${x+w-(fx-x)} ${fy}`,small?'#566575':'#9B9A92',seam);
    body+=line(`M${cx} ${junction}V${small?q((y+h*.80)/u):y+h*.80}`,small?'#566575':'#C2B9A9',seam);
  }
  if(v.startsWith('signal-')) {
    const badge=v.slice(7), unit=s/logical;
    const b=small?Math.max(5*unit,q(28)):q(23), bx=cx-b/2, by=junction-b*.38;
    const navy='#19334B', cream='#FFF9EC', halo=small?unit:q(1.6);
    if(badge==='shield') {
      const d=`M${cx} ${by-b*.12}Q${cx-b*.18} ${by+b*.04} ${bx} ${by+b*.09}V${by+b*.40}Q${bx} ${by+b*.87} ${cx} ${by+b*1.09}Q${bx+b} ${by+b*.87} ${bx+b} ${by+b*.40}V${by+b*.09}Q${cx+b*.18} ${by+b*.04} ${cx} ${by-b*.12}Z`;
      body+=`<path d="${d}" fill="${navy}" stroke="${cream}" stroke-width="${halo}" stroke-linejoin="round" paint-order="stroke"/>`;
    } else if(badge==='clasp') {
      body+=line(`M${cx-b*.27} ${by+b*.12}V${by-b*.15}A${b*.27} ${b*.27} 0 0 1 ${cx+b*.27} ${by-b*.15}V${by+b*.12}`,cream,small?unit*3:q(4.4));
      body+=line(`M${cx-b*.27} ${by+b*.12}V${by-b*.15}A${b*.27} ${b*.27} 0 0 1 ${cx+b*.27} ${by-b*.15}V${by+b*.12}`,navy,small?unit:q(2.8));
      body+=rect(bx,by,b,b*.84,small?unit:q(3),navy,`stroke="${cream}" stroke-width="${halo}" paint-order="stroke"`);
    } else {
      body+=`<circle cx="${cx}" cy="${by+b*.40}" r="${b*.56}" fill="${navy}" stroke="${cream}" stroke-width="${halo}"/>`;
      if(!small) body+=`<circle cx="${cx}" cy="${by+b*.40}" r="${b*.46}" fill="none" stroke="#597287" stroke-width="${q(.7)}"/>`;
    }
    if(logical>=48) {
      const ky=by+b*.33, kr=b*.125;
      body+=`<circle cx="${cx}" cy="${ky}" r="${kr}" fill="${cream}"/><path d="M${cx-kr*.45} ${ky+kr*.4}L${cx-kr*.72} ${ky+kr*2.3}H${cx+kr*.72}L${cx+kr*.45} ${ky+kr*.4}Z" fill="${cream}"/>`;
    } else if(logical>=24) {
      // A single sharp ivory slot replaces the keyhole in compact masters.
      const slotX=Math.round(cx/unit-.5)*unit, slotY=Math.round((by+b*.28)/unit)*unit;
      body+=rect(slotX,slotY,unit,2*unit,0,cream);
    }
  }
  return `<svg xmlns="http://www.w3.org/2000/svg" width="${s}" height="${s}" viewBox="0 0 ${s} ${s}"><title>ynotbit ${v} ${role}, ${logical}px optical master</title><defs><linearGradient id="paper" x2="0" y2="1"><stop stop-color="#FFFDF5"/><stop offset="1" stop-color="#EDE6D5"/></linearGradient><linearGradient id="flap" x2="0" y2="1"><stop stop-color="#FFFDF7"/><stop offset="1" stop-color="#F5EFDF"/></linearGradient><linearGradient id="navy" x2=".7" y2="1"><stop stop-color="#345A7A"/><stop offset=".5" stop-color="#193650"/><stop offset="1" stop-color="#0D2135"/></linearGradient><clipPath id="env">${rect(x,y,w,h,radius,'white')}</clipPath></defs>${body}</svg>`;
}
async function exportIcon(v,s,role,platform,logical=s,dest) {
  const rel=dest||`${v}/${role}/${platform}/${s}.png`;
  const svg=artwork(v,s,role,platform,logical);
  write(path.join(root,rel.replace(/\.png$/,'.svg')),svg);
  const png=await sharp(Buffer.from(svg),{density:72}).png().toBuffer();
  write(path.join(root,rel),png);
  manifest.push({file:rel,width:s,height:s,logicalSize:logical,variant:v,role,platform});
  return png;
}
function ico(files,dest) {
  const header=Buffer.alloc(6+16*files.length);header.writeUInt16LE(1,2);header.writeUInt16LE(files.length,4);
  let offset=header.length;
  files.forEach(({size,data},i)=> {const a=6+16*i;header[a]=size===256?0:size;header[a+1]=header[a];header.writeUInt16LE(1,a+4);header.writeUInt16LE(32,a+6);header.writeUInt32LE(data.length,a+8);header.writeUInt32LE(offset,a+12);offset+=data.length;});
  write(dest,Buffer.concat([header,...files.map(f=>f.data)]));
}
async function board() {
  const W=1500,H=1280;
  const items=[];
  let text='<text x="64" y="80" font-size="44" font-weight="700">ynotbit</text><text x="66" y="119" font-size="14" letter-spacing="3" fill="#677381">AIRMAIL · VECTOR EDITIONS</text>';
  for(let i=0;i<3;i++) {
    const v=variants[i], bx=50+i*480;
    items.push({input:await sharp(Buffer.from(artwork(v,280))).png().toBuffer(),left:bx+84,top:155});
    text+=`<text x="${bx+20}" y="490" font-size="27" font-weight="600">0${i+1} / ${v.replace('signal-','').toUpperCase()}</text>`;
    text+=`<text x="${bx+20}" y="528" font-size="17" fill="#617081">${(secure?['Signal + shield · protected mail','Signal + clasp · locked correspondence','Signal + seal · private letters']:['Paper folds · classic postal rhythm','Navy enamel · ivory correspondence','Y-fold · bold minimal signature'])[i]}</text>`;
    text+=`<rect x="${bx}" y="567" width="450" height="142" rx="16" fill="#E6EBEF"/><rect x="${bx}" y="726" width="450" height="142" rx="16" fill="#192B3E"/>`;
    for(const [j,s] of [16,20,24,32,48,64].entries()) {
      const left=bx+22+j*68;
      for(const yy of [603,762]) items.push({input:fs.readFileSync(path.join(root,v,'window','universal',`${s}.png`)),left,top:yy+Math.round((64-s)/2)});
      text+=`<text x="${left}" y="697" font-size="12" fill="#617081">${s}</text>`;
    }
    text+=`<text x="${bx+20}" y="918" font-size="13" letter-spacing="2" fill="#617081">16 / 24 / 32 PX — 4× PIXEL PROOF</text>`;
    for(const [j,s] of [16,24,32].entries()) {
      items.push({input:await sharp(fs.readFileSync(path.join(root,v,'window','universal',`${s}.png`))).resize(s*4,s*4,{kernel:'nearest'}).png().toBuffer(),left:bx+22+j*133,top:945+Math.round((128-s*4)/2)});
    }
  }
  text+='<text x="65" y="1181" font-size="18" fill="#617081">APPLICATION + WINDOW ICONS / macOS · Windows · Linux</text><text x="65" y="1215" font-size="15" fill="#617081">Small sizes use optical masters. Exported PNGs shown at native pixel size above.</text>';
  const bg=Buffer.from(`<svg xmlns="http://www.w3.org/2000/svg" width="${W}" height="${H}"><rect width="100%" height="100%" fill="#F7F8F7"/><g font-family="Helvetica,Arial,sans-serif" fill="#19334B">${text}</g></svg>`);
  await sharp(bg).composite(items).png().toFile(path.join(root,'comparison.png'));
}
async function main() {
  for(const v of variants) {
    for(const role of ['application','window']) {
      const frames=[];
      for(const s of sizes) {
        const png=await exportIcon(v,s,role,'universal');
        if(s<=256) frames.push({size:s,data:png});
      }
      ico(frames,path.join(root,v,'windows',`ynotbit-${role}.ico`));
    }
    for(const s of [16,32,128,256,512]) for(const scale of [1,2]) {
      await exportIcon(v,s*scale,'application','macos',s,`${v}/macos/ynotbit.iconset/icon_${s}x${s}${scale===2?'@2x':''}.png`);
    }
    // iconutil expects PNG-only iconsets; SVG sources are stored alongside.
    const iconset=path.join(root,v,'macos/ynotbit.iconset');
    const masters=path.join(root,v,'macos/optical-masters');fs.mkdirSync(masters,{recursive:true});
    for(const f of fs.readdirSync(iconset).filter(f=>f.endsWith('.svg'))) fs.renameSync(path.join(iconset,f),path.join(masters,f));
    if(process.platform==='darwin') cp.execFileSync('iconutil',['-c','icns',iconset,'-o',path.join(root,v,'macos/ynotbit.icns')]);
    for(const s of sizes.filter(s=>s<=512)) {
      const out=path.join(root,v,'linux/hicolor',`${s}x${s}/apps/ynotbit.png`);
      write(out,fs.readFileSync(path.join(root,v,'application/universal',`${s}.png`)));
    }
    write(path.join(root,v,'linux/hicolor/scalable/apps/ynotbit.svg'),artwork(v,256));
  }
  write(path.join(root,'manifest.json'),JSON.stringify(manifest,null,2)+'\n');
  await board();
  console.log(`Exported ${manifest.length} PNGs, 6 ICOs, 3 ICNS, SVG masters and comparison.png`);
}
main().catch(e=>{console.error(e);process.exit(1)});
