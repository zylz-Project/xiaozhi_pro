/* Device-backed actions for the WZH detail page. */
(function(){
'use strict';
const $=s=>document.querySelector(s), $$=s=>Array.from(document.querySelectorAll(s));
const sleep=n=>new Promise(r=>setTimeout(r,n));
let catalog=[], current=null, selectedFile=null, uploadMode=false, working=false, lastOffline=0, statusBusy=false;
async function api(path,body,options={}) {const o={cache:'no-store',credentials:'same-origin',...options};if(body!==undefined){o.method='POST';o.headers={'Content-Type':'application/json'};o.body=JSON.stringify(body);}const r=await fetch(path,o),d=await r.json();if(!r.ok||d.ok===false){const e=new Error(d.error?.message||d.error||'操作失败');e.status=r.status;throw e;}return d;}
function toast(s){const p=$('.phone');if(p?.__toast)p.__toast(s);else alert(s);}
function textIfChanged(el,value){value=String(value);if(el&&el.textContent!==value)el.textContent=value;}
function paint(s){
 const state=s.state||'idle', labels={idle:'待机',listening:'聆听中',speaking:'播放中',connecting:'连接中',upgrading:'升级中',wifi_configuring:'配网中'};
 $$('.status-text,.sheet-state').forEach(e=>textIfChanged(e,labels[state]||'设备忙')); const p=$('.phone');if(p.dataset.state!==state)p.dataset.state=state;
 const cta=$('.sheet-main > .primary'),disabled=!['idle','listening','speaking'].includes(state);if(cta.disabled!==disabled)cta.disabled=disabled;textIfChanged($('.cta-label'),['listening','speaking'].includes(state)?'结束对话':'开始对话');
 const b=s.battery?.available?s.battery.percent:null,t=s.temperature_c, w=s.wifi||{};
 const metrics=$$('.metrics .metric');if(metrics[0]){textIfChanged(metrics[0].querySelector('b'),b==null?'--':b+'%');textIfChanged(metrics[0].querySelector('em'),s.battery?.charging?'充电中':'电池');}if(metrics[1]){textIfChanged(metrics[1].querySelector('b'),t==null?'--':Number(t).toFixed(1)+'°C');textIfChanged(metrics[1].querySelector('em'),'芯片温度');}
 textIfChanged($('.battery-core b'),b==null?'--':b);textIfChanged($('.battery-caption'),s.battery?.charging?'电量 · 充电中':'电量');textIfChanged($('.readout-copy b'),t==null?'--':Number(t).toFixed(1)+'°C');const readouts=$$('.readout-copy b');if(readouts[1])textIfChanged(readouts[1],Math.floor((s.uptime_s||0)/60)+' 分钟');
 $$('[data-metric-net],.net-name,.net-val,.more-state').forEach(e=>textIfChanged(e,w.ssid||'--'));$$('.net-sub,[data-metric-signal]').forEach(e=>textIfChanged(e,w.connected?(w.rssi==null?'已连接':w.rssi+' dBm'):'离线'));$$('.net-chip').forEach(e=>{textIfChanged(e,'--');if(e.getAttribute('aria-label')!=='延迟不可用')e.setAttribute('aria-label','延迟不可用');});$$('[data-p-version]').forEach(e=>textIfChanged(e,s.version||'--'));
 $$('.volume').forEach(e=>{if(document.activeElement!==e&&s.audio?.volume!=null){if(e.value!==String(s.audio.volume))e.value=s.audio.volume;if(e.style.getPropertyValue('--val')!==e.value+'%')e.style.setProperty('--val',e.value+'%');textIfChanged(e.parentNode.querySelector('.volume-val'),e.value);}});
 if(!w.connected){if(!lastOffline)lastOffline=Date.now();if(Date.now()-lastOffline>3000)location.href='/setup';}else lastOffline=0;
 if(window.PandaProduct){const id=s.build==='wzh-dinosaur'?'dinosaur':s.build==='wzh-tailredpanda'?'redpanda':'crawlPanda';if(PandaProduct.get().id!==id)PandaProduct.set(id,false);}
}
async function status(){if(statusBusy)return;statusBusy=true;const controller=new AbortController();const timer=setTimeout(()=>controller.abort(),10000);try{paint(await api('/api/status',undefined,{signal:controller.signal}));}catch(e){if(e.status===401)location.href='/login';}finally{clearTimeout(timer);statusBusy=false;}}
async function voices(){try{let d=await api('/api/user/voices');if(d.voice_management_busy){setTimeout(voices,1500);return;}catalog=d.voices||[];current=catalog.find(v=>v.current)||null;const label=current?.name|| (d.has_key===false?'语音服务未配置':'暂无音色');$$('.voice-now,.voice-meta b,[data-vm-current]').forEach(e=>e.textContent=label);
 const fixed=$('.fixed-track'), rail=$('.clone-rail'), list=$('[data-vm-list]');fixed.textContent='';rail.querySelectorAll('.clone-chip').forEach(e=>e.remove());list.textContent='';
 for(const v of catalog){const chip=document.createElement('button');chip.type='button';chip.className='voice-chip'+(v.builtin?'':' clone-chip')+(v.current?' on':'');chip.textContent=v.name;chip.dataset.voiceId=v.id;if(v.builtin)fixed.append(chip);else{rail.insertBefore(chip,rail.querySelector('[data-voice-add]'));const row=document.createElement('div');row.className='vm-row';const name=document.createElement('b');name.textContent=v.name;row.append(name);for(const [label,act] of [['使用','select'],['试听','audition'],['改名','rename'],['删除','delete']]){const b=document.createElement('button');b.type='button';b.className='vm-use';b.textContent=label;b.dataset.voiceId=v.id;b.dataset.voiceAction=act;row.append(b);}list.append(row);}}
 const n=catalog.filter(v=>!v.builtin).length;$$('[data-vm-count],[data-manage-count],.lab-count').forEach(e=>e.textContent=n);if(d.has_key===false)toast('语音服务未配置，请联系管理员');
 }catch(e){toast(e.message);}}
async function action(act,id){let name='';if(act==='rename'){name=prompt('请输入新名称');if(!name)return;}if(act==='delete'&&!confirm('确认删除云端专属音色？'))return;await api('/api/user/voices/'+act,{id,name});if(act!=='audition'){await sleep(1000);voices();}else toast('试听请求已提交');}
async function record(){const name=$('[data-audio-name-input]').value.trim();if(!name){toast('请先输入音色名称');return;}await api('/api/user/voices/device-record',{name});toast('请听设备提示，靠近设备说话');for(let i=0;i<180;i++){await sleep(1000);const s=await api('/api/user/voices/device-record/status');$('[data-record-status]').textContent={starting:'正在准备',prompt:'请听设备提示',recording:'设备正在录音',processing:'正在创建音色',success:'创建成功',failed:'创建失败',cancelled:'已取消'}[s.phase]||s.phase;if(['success','failed','cancelled'].includes(s.phase)){voices();return;}}toast('处理仍在进行，请稍后刷新');}
async function upload(){const name=$('[data-audio-name-input]').value.trim();if(!name||!selectedFile)throw new Error('请选择录音并输入名称');if(selectedFile.size>20*1024*1024)throw new Error('文件超过 20 MB');const AC=window.AudioContext||window.webkitAudioContext,Offline=window.OfflineAudioContext||window.webkitOfflineAudioContext;if(!AC||!Offline)throw new Error('浏览器不支持音频处理');const a=new AC();let d;try{d=await a.decodeAudioData(await selectedFile.arrayBuffer());}finally{await a.close();}if(d.duration<5||d.duration>20.05)throw new Error('录音需要 5–20 秒');const render=new Offline(1,Math.min(320000,Math.round(d.duration*16000)),16000),src=render.createBufferSource();src.buffer=d;src.connect(render.destination);src.start();const samples=(await render.startRendering()).getChannelData(0),pcm=new ArrayBuffer(samples.length*2),view=new DataView(pcm);samples.forEach((v,i)=>view.setInt16(i*2,Math.round(Math.max(-1,Math.min(1,v))*32767),true));await api('/api/user/voices/clone',undefined,{method:'POST',headers:{'Content-Type':'application/octet-stream','X-Voice-Name':encodeURIComponent(name)},body:pcm});toast('正在创建音色');for(let i=0;i<180;i++){await sleep(1000);const s=await api('/api/user/voices/clone/status');if(s.phase==='done'){toast('创建成功');voices();return;}if(s.phase==='failed')throw new Error('创建失败，请检查语音服务');}toast('仍在处理，请稍后刷新');}
async function reset(){const confirm=$('[data-reset-input]').value.trim();if(confirm!=='确定恢复出厂设置')return;await api('/api/user/factory-reset',{confirm});toast('正在恢复，请保持设备供电');for(;;){await sleep(1000);try{const s=await api('/api/user/factory-reset/status');toast(s.error?.message||({checking:'正在检查',deleting_cloud:'正在删除云端音色',clearing_local:'清理本地音色',clearing_wifi:'清除网络',rebooting:'设备正在重启',failed:'恢复失败'}[s.phase]||'恢复中'));if(s.phase==='failed')return;}catch(e){toast('连接中断，请重新连接设备热点查看结果');return;}}}
document.addEventListener('click',async e=>{const target=e.target.closest('button');if(!target)return;
 if(target.matches('[data-create-tab]')){uploadMode=target.dataset.createTab==='upload';setTimeout(()=>{$('[data-create-audio]').disabled=false;$('[data-record-status]').textContent='输入名称后点击设备录音';},0);return;}
 if(target.matches('[data-voice-add],.lab-record,[data-vm-new],.lab-upload')){uploadMode=target.matches('.lab-upload');setTimeout(()=>{$('[data-create-audio]').disabled=false;$('[data-record-status]').textContent='输入名称后点击设备录音';},0);return;}
 let task=null;if(target.matches('.sheet-main > .primary,.talk-cta,.float-assistant'))task=()=>api('/api/user/chat/toggle',{}).then(status);
 else if(target.matches('.manual-ribbon'))task=()=>location.href='/manual';
 else if(target.matches('.net-change'))task=()=>{location.href='/setup?from=device';};
 else if(target.matches('.voice-try,.listen-mini,[data-vm-audition]'))task=()=>current?action('audition',current.id):Promise.reject(new Error('没有可试听的音色'));
 else if(target.dataset.voiceId)task=()=>action(target.dataset.voiceAction||'select',target.dataset.voiceId);
 else if(target.matches('[data-record-puck]'))task=record;
 else if(target.matches('[data-create-audio]'))task=()=>uploadMode?upload():record();
 else if(target.matches('[data-reset-confirm]'))task=reset;
 if(!task)return;e.preventDefault();e.stopImmediatePropagation();if(working){toast('设备正在处理，请稍候');return;}working=true;try{await task();}catch(err){toast(err.message||'操作失败');}finally{working=false;}
},true);
document.addEventListener('change',e=>{if(e.target.matches('.audio-modal input[type=file]')){e.stopImmediatePropagation();selectedFile=e.target.files?.[0]||null;$('[data-audio-file]').textContent=selectedFile?.name||'选择音频';$('[data-create-audio]').disabled=!selectedFile;}else if(e.target.matches('.volume')){api('/api/user/volume',{volume:Number(e.target.value)}).catch(err=>toast(err.message));}},true);
document.addEventListener('DOMContentLoaded',()=>{
  // Clear prototype sample values before reading the device. Never expose demo data as telemetry.
  $$('.metrics .metric b,.battery-core b,.readout-copy b,.net-chip,.net-name,.net-val,[data-metric-net],.voice-now,.voice-meta b,[data-vm-current]').forEach(e=>e.textContent='--');
  $$('.clone-chip').forEach(e=>e.remove());
  const list=$('[data-vm-list]'); if(list) list.textContent='';
  const track=$('.fixed-track'); if(track) track.textContent='';
  const trim=$('.audio-trim'); if(trim) trim.hidden=true; // The upload API accepts the complete 5–20 s clip.
  status();voices();setInterval(()=>{if(!document.hidden)status();},5000);
});
})();
