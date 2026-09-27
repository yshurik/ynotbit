const fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict');
const sharp=require('sharp');
const base=__dirname,src=path.join(base,'protected/signal-shield'),out=path.join(base,'final');
fs.mkdirSync(out,{recursive:true});
fs.cpSync(src,out,{recursive:true});
const entries=JSON.parse(fs.readFileSync(path.join(base,'protected/manifest.json'))).filter(x=>x.variant==='signal-shield').map(x=>({...x,file:x.file.replace('signal-shield/','')}));
fs.writeFileSync(path.join(out,'manifest.json'),JSON.stringify(entries,null,2));
function svg(t,w,h){return Buffer.from(`<svg xmlns="http://www.w3.org/2000/svg" width="${w}" height="${h}">${t}</svg>`)}
async function main(){
  const assets=[];
  let t='<rect width="100%" height="100%" fill="#F5F5F1"/><g font-family="Helvetica,Arial,sans-serif" fill="#19334B"><text x="56" y="72" font-size="40" font-weight="700">ynotbit / Signal + Shield</text><text x="58" y="110" font-size="16" letter-spacing="2" fill="#657382">PROTECTED MAIL · FINAL ICON FAMILY</text>';
  for(const [i,p] of ['macOS','Windows','Linux'].entries()){
    const left=70+i*375;
    const file=i===0?'macos/ynotbit.iconset/icon_128x128@2x.png':'application/universal/256.png';
    assets.push({input:fs.readFileSync(path.join(out,file)),left:left+32,top:140});
    t+=`<text x="${left+38}" y="435" font-size="22" font-weight="600">${p}</text><text x="${left+38}" y="465" font-size="14" fill="#657382">${['ICNS + Retina optical masters','Application ICO + Window ICO','PNG + scalable SVG / hicolor'][i]}</text>`;
  }
  const sizes=[16,20,24,32,40,48,64,96];
  for(const [r,role] of ['application','window'].entries()){
    const yy=510+r*215;
    t+=`<text x="58" y="${yy}" font-size="14" letter-spacing="2">${role.toUpperCase()} / ACTUAL PIXELS</text><rect x="52" y="${yy+15}" width="1110" height="82" rx="12" fill="#E4E9EC"/><rect x="52" y="${yy+103}" width="1110" height="82" rx="12" fill="#172A3D"/>`;
    for(const [j,s] of sizes.entries()){
      const drawSize=Math.min(s,64), left=83+j*135;
      // 96 is shown at 96px in its own wider cell; all samples are native pixels.
      for(const y0 of [yy+16,yy+104])assets.push({input:fs.readFileSync(path.join(out,role,'universal',`${s}.png`)),left,top:y0+Math.round((80-s)/2)});
      t+=`<text x="${left+5}" y="${yy+202}" font-size="12" fill="#657382">${s} px</text>`;
    }
  }
  t+='<text x="58" y="985" font-size="14" letter-spacing="2">OPTICAL MASTERS / 16 · 24 · 32 PX AT 4×</text><text x="665" y="1017" font-size="17">16–20 px  /  solid shield</text><text x="665" y="1054" font-size="17">24–40 px  /  crisp ivory slot</text><text x="665" y="1091" font-size="17">48+ px  /  full keyhole detail</text></g>';
  for(const [i,s] of [16,24,32].entries())assets.push({input:await sharp(path.join(out,'window/universal',`${s}.png`)).resize(s*4,s*4,{kernel:'nearest'}).png().toBuffer(),left:80+i*180,top:1010+Math.round((128-s*4)/2)});
  await sharp(svg(t,1220,1180)).composite(assets).png().toFile(path.join(out,'preview.png'));
  const checks=[];
  for(const e of entries){
    const p=path.join(out,e.file),m=await sharp(p).metadata();
    assert.equal(m.width,e.width);assert.equal(m.height,e.height);assert.equal(m.hasAlpha,true);
    const {data,info}=await sharp(p).ensureAlpha().raw().toBuffer({resolveWithObject:true});
    assert.equal(data[3],0);assert.equal(data[(info.width*info.height-1)*4+3],0);
  }
  checks.push(`${entries.length} PNG optical exports: dimensions, alpha and transparent corners verified.`);
  for(const role of ['application','window']){
    const b=fs.readFileSync(path.join(out,'windows',`ynotbit-${role}.ico`));
    assert.equal(b.readUInt16LE(2),1);const count=b.readUInt16LE(4);assert.equal(count,10);
    const found=[];
    for(let i=0;i<count;i++){
      const a=6+16*i,s=b[a]||256,n=b.readUInt32LE(a+8),offset=b.readUInt32LE(a+12);
      assert.ok(offset+n<=b.length);const m=await sharp(b.subarray(offset,offset+n)).metadata();assert.equal(m.width,s);assert.equal(m.height,s);found.push(s);
    }
    checks.push(`${role} ICO: ${found.join(', ')} px, all 10 embedded images decoded.`);
  }
  const icns=fs.readFileSync(path.join(out,'macos/ynotbit.icns'));
  assert.equal(icns.toString('ascii',0,4),'icns');assert.equal(icns.readUInt32BE(4),icns.length);
  let at=8,n=0;while(at<icns.length){const len=icns.readUInt32BE(at+4);assert.ok(len>8&&at+len<=icns.length);at+=len;n++;}assert.equal(at,icns.length);
  checks.push(`ICNS: valid container length and ${n} resource blocks; built by Apple's iconutil.`);
  fs.writeFileSync(path.join(out,'verification.txt'),checks.join('\n')+'\n');
  fs.mkdirSync(path.join(out,'logo'),{recursive:true});
  fs.copyFileSync(path.join(out,'application/universal/1024.svg'),path.join(out,'logo/ynotbit-mark.svg'));
  const mark=fs.readFileSync(path.join(out,'application/universal/1024.svg'),'utf8').replace('width="1024" height="1024"','x="0" y="0" width="256" height="256"');
  const lockup=svg(`<rect width="100%" height="100%" fill="none"/>${mark}<text x="295" y="165" font-family="Helvetica,Arial,sans-serif" font-weight="700" font-size="116" letter-spacing="-4" fill="#19334B">ynotbit</text>`,760,256);
  fs.writeFileSync(path.join(out,'logo/ynotbit-lockup.svg'),lockup);
  console.log(checks.join('\n'));
}
main().catch(e=>{console.error(e);process.exit(1)});
