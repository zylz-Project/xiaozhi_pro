#!/usr/bin/env python3
"""Build a local audition page with targets exported by the production C++ player."""
import json
from pathlib import Path
from audit_wzh_choreography import audit, ROOT


def main():
    files, rows = audit()
    data = json.dumps({'files': files, 'trajectories': rows}, ensure_ascii=False, separators=(',', ':')).replace('<', '\\u003c')
    template = r'''<!doctype html>
<html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>萌宠 · 声音与动作预览</title>
<style>
*{box-sizing:border-box}body{margin:0;background:#f4f1eb;color:#243735;font:16px/1.6 system-ui,sans-serif}main{max-width:1000px;margin:40px auto;padding:24px}h1{font-size:32px;margin:0}p{color:#5a6c67}section{padding:26px;background:#fff;border-radius:24px;margin:20px 0;box-shadow:0 8px 28px #22363109}.row{display:flex;gap:12px;flex-wrap:wrap;align-items:center}button,select{font:inherit;border:1px solid #d9e1db;border-radius:12px;padding:10px 14px;background:#f9fbf8;color:#243735}button{cursor:pointer}button.active,#play{background:#286750;color:#fff;border-color:#286750}select{max-width:100%}audio{width:100%;margin-top:20px}canvas{width:100%;height:210px;background:#fbfaf7;border-radius:16px}.axis{display:grid;grid-template-columns:90px 1fr 56px;gap:12px;align-items:center;margin:12px 0}.track{background:#eef2ed;border-radius:20px;height:12px;overflow:hidden}.fill{height:100%;background:#dba864;transition:width .05s linear}.note{font-size:14px}#status{font-variant-numeric:tabular-nums;color:#286750}a{color:#286750}
</style><main><h1>萌宠 · 声音与动作</h1><p>听一段声音，看头、前爪与尾巴怎样一起回应。</p>
<section><div class="row" id="products"></div><p id="policy"></p><div class="row"><select id="sounds" aria-label="声音"></select><select id="variant" aria-label="动作变化"><option value="1">变化 1</option><option value="2">变化 2</option><option value="3">变化 3</option><option value="4">变化 4</option><option value="5">变化 5</option><option value="6">变化 6</option><option value="7">变化 7</option><option value="8">变化 8</option><option value="9">变化 9</option><option value="10">变化 10</option><option value="11">变化 11</option><option value="12">变化 12</option><option value="13">变化 13</option><option value="14">变化 14</option><option value="15">变化 15</option><option value="16">变化 16</option></select><button id="play">开始试听</button><button id="stop">停止</button></div><audio id="audio" controls preload="metadata"></audio><p id="status" aria-live="polite"></p></section>
<section><canvas id="chart" width="1600" height="360"></canvas><div id="axes"></div><p class="note">曲线和关节角度来自固件动作播放器，图示用于核对编排。实机的声音延迟、惯性与机械空间需装机确认。试听使用本地音频文件，不上传音频。</p></section>
<p class="note"><a href="wzh_audio_choreography_2026-10-19.md">查看逐文件配对与展示说明</a> · 若声音无法加载，请从工程目录直接用浏览器打开此文件，或在项目的上级目录启动本地 HTTP 文件服务。</p></main>
<script>
const pack=__DATA__;
const $=id=>document.getElementById(id), audio=$('audio');
const products=[['crawling_panda','大熊猫',['头部','左前爪','右前爪']],['tail_wagging_panda','小熊猫',['头部','尾巴左右','尾巴上下']],['dinosaur','恐龙',['颈部俯仰','颈部侧倾','头部转向','尾巴上下','尾巴左右']]];
const colors=['#3f7c65','#dca14e','#7487bd','#b56d8f','#85a458'];
let product=products[0],trajectory=null,finishAt=0,finishTime=0,stopped=true;
function selectProduct(p){product=p;audio.pause();stopped=true;finishAt=0;$('sounds').replaceChildren();pack.files.map((f,i)=>({f,i})).sort((a,b)=>(a.f.category==='ambient')-(b.f.category==='ambient')).forEach(({f,i})=>{if(f.path.startsWith(p[0]+'/')){let o=document.createElement('option');o.value=i;o.textContent=f.name+(f.category==='ambient'?'（环境音 · 手动试听）':'');$('sounds').append(o)}});$('products').querySelectorAll('button').forEach(b=>b.classList.toggle('active',b.textContent===p[1]));$('policy').textContent=p[0]==='dinosaur'?'展示节奏：正常空闲约2秒后回应；环境音自动播放12秒片段。':'展示节奏：18段动物音不重复轮播；长环境音只保留手动试听。';load();}
function load(){audio.pause();stopped=true;finishAt=0;const i=+$('sounds').value;trajectory=pack.trajectories.find(t=>t.file===i&&t.variant===+$('variant').value);audio.src='../../final/audio_serverWeb/audio_files/'+pack.files[i].path.split('/').map(encodeURIComponent).join('/');$('axes').replaceChildren();product[2].forEach((name,j)=>{const r=document.createElement('div');r.className='axis';const n=document.createElement('span');n.textContent=name;const track=document.createElement('div');track.className='track';const fill=document.createElement('div');fill.className='fill';fill.style.background=colors[j];track.append(fill);const value=document.createElement('span');value.className='value';r.append(n,track,value);$('axes').append(r)});}
products.forEach(p=>{const b=document.createElement('button');b.textContent=p[1];b.onclick=()=>selectProduct(p);$('products').append(b)});
$('sounds').onchange=load;$('variant').onchange=load;
$('play').onclick=async()=>{stopped=false;finishAt=0;try{audio.currentTime=0;await audio.play()}catch(e){$('status').textContent='未能打开本地音频：'+e.message;stopped=true}};
$('stop').onclick=()=>{audio.pause();audio.currentTime=0;finishAt=0;stopped=true};
audio.onplay=()=>{stopped=false;finishAt=0};audio.onended=()=>{finishAt=performance.now();finishTime=audio.currentTime*1000};
function draw(){if(!trajectory)return requestAnimationFrame(draw);let ms=stopped?0:audio.currentTime*1000;if(finishAt)ms=Math.min(trajectory.motion_ms,finishTime+performance.now()-finishAt);const points=trajectory.preview;const point=points[Math.min(points.length-1,Math.floor(ms/100))];$('axes').querySelectorAll('.axis').forEach((r,j)=>{const value=point[j+2];r.querySelector('.fill').style.width=(value/180*100)+'%';r.querySelector('.value').textContent=value.toFixed(0)+'°'});$('status').textContent=`声音 ${(trajectory.audio_ms/1000).toFixed(2)} 秒 · 动作 ${(trajectory.motion_ms/1000).toFixed(2)} 秒 · 当前 ${(ms/1000).toFixed(1)} 秒`+(point[1]?'':' · 放松停顿');const c=$('chart'),ctx=c.getContext('2d'),w=c.width,h=c.height;ctx.clearRect(0,0,w,h);ctx.strokeStyle='#e8ebe5';ctx.lineWidth=1;for(let d=0;d<=180;d+=45){const y=20+(180-d)/180*(h-40);ctx.beginPath();ctx.moveTo(0,y);ctx.lineTo(w,y);ctx.stroke()}product[2].forEach((_,j)=>{ctx.strokeStyle=colors[j];ctx.lineWidth=4;ctx.beginPath();points.forEach((p,i)=>{const x=p[0]/trajectory.motion_ms*w,y=20+(180-p[j+2])/180*(h-40);i?ctx.lineTo(x,y):ctx.moveTo(x,y)});ctx.stroke()});ctx.strokeStyle='#243735';ctx.lineWidth=3;const x=Math.min(ms/trajectory.motion_ms,1)*w;ctx.beginPath();ctx.moveTo(x,0);ctx.lineTo(x,h);ctx.stroke();requestAnimationFrame(draw)}
selectProduct(product);requestAnimationFrame(draw);
</script></html>'''
    target = ROOT/'docs/wzh_audio_motion_preview.html'
    target.write_text(template.replace('__DATA__',data))
    print(target)


if __name__ == '__main__':
    main()
