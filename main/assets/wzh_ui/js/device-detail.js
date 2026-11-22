/* Device-backed actions for the WZH detail page. */
(function(){
'use strict';
const $=s=>document.querySelector(s), $$=s=>Array.from(document.querySelectorAll(s));
const sleep=n=>new Promise(r=>setTimeout(r,n));
let catalog=[], current=null, working=false, statusBusy=false, statusTimer=0, lastStatus=null;
let voiceBusy=false, voiceRetry=0, catalogLoaded=false, refreshCatalog=true, statusPending=false;
let auditionPending=false, chatBusy=false, voiceJob=null, voiceAvailable=null, volumePending=null;
let volumeWriting=false, lastVolume=null;
let voiceSignature='';
const products={'wzh-crawlpanda':'crawlPanda','wzh-tailredpanda':'redpanda','wzh-dinosaur':'dinosaur'};
async function api(path,body,options={}) {
 const {timeout=10000,...request}=options;
 const controller=new AbortController(),timer=setTimeout(()=>controller.abort(),timeout);
 try{
  const o={cache:'no-store',credentials:'same-origin',...request,signal:controller.signal};
  if(body!==undefined){o.method='POST';o.headers={'Content-Type':'application/json'};o.body=JSON.stringify(body);}
  const r=await fetch(path,o),d=await r.json().catch(()=>null);
  if(!r.ok||d?.ok===false){const e=new Error(d?.error?.message||d?.error||'操作失败');e.status=r.status;throw e;}
  if(!d||typeof d!=='object'||Array.isArray(d))throw new Error('设备回复未接收完整，请稍后重试');
  return d;
 }catch(e){
  if(e.name==='AbortError'){const x=new Error('连接设备超时，请确认手机仍连接设备热点后重试');x.timeout=true;throw x;}
  throw e;
 }finally{clearTimeout(timer);}
}
function toast(s){const p=$('.phone');if(p?.__toast)p.__toast(s);else alert(s);}
function loginFor(e){if(e&&e.status===401){location.href='/login?next=/';return true;}return false;}
function textIfChanged(el,value){value=String(value);if(el&&el.textContent!==value)el.textContent=value;}
function loadBrandMark(){
 const mark=$('.brand-mark[data-src]');
 if(!mark)return;
 mark.src=mark.dataset.src;
 mark.removeAttribute('data-src');
}
function networkOnline(s){
 return s.transport!=='probing' && (s.transport==='cellular'||s.cellular
  ? !!s.cellular?.connected : !!s.wifi?.connected);
}
function paintChat(s){
 const state=s?.state, intro=!!s?.conversation_intro, recovering=!!s?.conversation_recovering;
 const active=recovering||['connecting','listening','speaking'].includes(state);
 const available=state==='idle'&&networkOnline(s)&&!s.onboarding_blocked&&s.conversation_configured!==false;
 const label=chatBusy?'请稍候…':recovering||state==='connecting'?'取消连接':active?'结束对话':'开始对话';
 const hint=!s?'正在读取设备状态…':recovering?'正在恢复对话连接，可以取消。':intro?'正在播放首次对话教程，播完就会开始聆听。':
  state==='connecting'?'正在连接对话服务…':
  state==='activating'?'正在连接对话服务，可以先离线玩耍。':
  state==='idle'&&s.onboarding_blocked?'设备正在完成开机教程。':
  state==='idle'&&!networkOnline(s)?'设备暂未联网，可以继续离线玩耍。':
  state==='idle'&&s.conversation_configured===false?'对话服务暂未就绪，可以先离线玩耍。':'';
 textIfChanged($('.cta-label'),label);
 $$('.sheet-main > .primary,.talk-cta,.float-assistant').forEach(b=>{
  b.disabled=chatBusy||(!active&&!available);
  b.setAttribute('aria-label',label);
 });
 const notice=$('#chatNotice');
 if(notice){textIfChanged(notice,hint);notice.hidden=!hint;}
}
function paint(s){
 const state=s.state||'idle', labels={idle:'待机中',listening:'聆听中',speaking:'说话中',connecting:s.conversation_intro?'新手教程':'连接中',activating:'服务准备中',upgrading:'升级中',wifi_configuring:'配网中'};
 $$('.status-text,.sheet-state').forEach(e=>textIfChanged(e,s.conversation_recovering?'正在恢复对话':labels[state]||'设备忙'));const notice=$('#startupNotice');if(notice){textIfChanged(notice,s.startup_notice||'');notice.hidden=!s.startup_notice;} const p=$('.phone');if(p.dataset.state!==state)p.dataset.state=state;
 paintChat(s);
 const b=s.battery?.available?s.battery.percent:null,t=s.temperature_c, w=s.wifi||{};
 const metrics=$$('.metrics .metric');if(metrics[0]){textIfChanged(metrics[0].querySelector('b'),b==null?'--':b+'%');textIfChanged(metrics[0].querySelector('em'),s.battery?.charging?'充电中':'电池');}if(metrics[1]){textIfChanged(metrics[1].querySelector('b'),t==null?'--':Number(t).toFixed(1)+'°C');textIfChanged(metrics[1].querySelector('em'),'芯片温度');}
 textIfChanged($('.battery-core b'),b==null?'--':b);textIfChanged($('.battery-caption'),s.battery?.charging?'电量 · 充电中':'电量');textIfChanged($('.readout-copy b'),t==null?'--':Number(t).toFixed(1)+'°C');const readouts=$$('.readout-copy b');if(readouts[1])textIfChanged(readouts[1],Math.floor((s.uptime_s||0)/60)+' 分钟');
 const tr=(s.transport||''), probing=tr==='probing', cell=tr==='cellular'||(tr===''&&s.cellular), netName=probing?'检测网络':(cell?'4G':(w.ssid||'--')), netState=probing?'正在检测模块':(cell?(!s.cellular?.connected?'网络离线':s.cellular?.server_connected?'服务端已连接':'网络已连接 · 服务端未连接'):(w.connected?(w.rssi==null?'已连接':w.rssi+' dBm'):'离线'));$$('[data-metric-net],.net-name,.net-val,.more-state').forEach(e=>textIfChanged(e,netName));$$('.net-sub,[data-metric-signal]').forEach(e=>textIfChanged(e,netState));$$('.net-chip').forEach(e=>{textIfChanged(e,'--');if(e.getAttribute('aria-label')!=='延迟不可用')e.setAttribute('aria-label','延迟不可用');});$$('[data-p-version]').forEach(e=>textIfChanged(e,s.version||'--'));
 if(window.__renderTransportSwitch)window.__renderTransportSwitch({probing,cell,switchable:!!s.transport_switchable,connected:cell?!!s.cellular?.connected:!!w.connected});
 if(!volumeWriting&&s.audio?.volume!=null){lastVolume=s.audio.volume;paintVolume(lastVolume);}
 paintVoiceService();
}
async function status(){
 if(statusBusy||document.hidden)return;
 clearTimeout(statusTimer);
 statusBusy=true;
 try{
  const s=await api('/api/status');
  const product=products[s.build];
  if(!product||typeof s.state!=='string'||typeof s.wifi?.connected!=='boolean')
   throw new Error('设备状态未接收完整');
  const builtFor=document.documentElement.dataset.buildProduct;
  if(builtFor&&builtFor!==product){
   const url=new URL(location.href);
   if(url.searchParams.get('_device')!==product){
    url.searchParams.set('_device',product);
    location.replace(url.href);
   }else{textIfChanged($('.status-text'),'请重新打开设备页面');}
   return;
  }
  window.PandaProduct?.updateDeviceStatus?.(s);
  if(lastStatus&&!networkOnline(lastStatus)&&networkOnline(s))refreshCatalog=true;
  lastStatus=s;
  paint(s);
  loadBrandMark();
  if(!auditionPending&&(refreshCatalog||!catalogLoaded)&&(s.wifi.connected||s.cellular?.connected)){
   refreshCatalog=false;
   voices(true);
  }
 }catch(e){
  if(!loginFor(e)){
   $$('.status-text,.sheet-state').forEach(el=>textIfChanged(el,'正在恢复连接…'));
   loadBrandMark();
  }
 }finally{
  statusBusy=false;
  if(statusPending){statusPending=false;status();}
  else if(!document.hidden)statusTimer=setTimeout(status,lastStatus?.state==='connecting'||lastStatus?.conversation_recovering?1000:5000);
 }
}
function resumeStatus(){
 if(document.hidden)return;
 clearTimeout(statusTimer);
 refreshCatalog=true;
 if(statusBusy)statusPending=true;
 else status();
}
function paintVolume(volume){
 $$('.volume').forEach(slider=>{
  if(document.activeElement===slider)return;
  if(slider.value!==String(volume))slider.value=volume;
  if(slider.style.getPropertyValue('--val')!==slider.value+'%')
   slider.style.setProperty('--val',slider.value+'%');
  textIfChanged(slider.parentNode.querySelector('.volume-val'),slider.value);
 });
}
async function setVolume(volume){
 volumePending=volume;
 if(volumeWriting)return;
 volumeWriting=true;
 try{
  while(volumePending!==null){
   const next=volumePending;
   volumePending=null;
   const result=await api('/api/user/volume',{volume:next});
   lastVolume=result.volume??next;
  }
 }catch(e){
  if(!loginFor(e))toast(e.message);
 }finally{
  volumeWriting=false;
  volumePending=null;
  if(lastVolume!==null)paintVolume(lastVolume);
 }
}
function paintVoiceService(){
 const notice=$('#voiceServiceNotice');
 const hint=lastStatus&&!networkOnline(lastStatus)?'设备联网后可以复刻和试听音色。':
  voiceAvailable===false?'音色服务尚未配置，原声仍可使用。':'';
 if(notice){textIfChanged(notice,hint);notice.hidden=!hint;}
}
function paintCapacity(n){n=Number(n)||0;const cap=document.querySelector('[data-sound-cap]');if(cap)cap.classList.toggle('is-full',n>=10);const num=document.querySelector('[data-cap-num]');if(num)num.textContent=n;const fill=document.querySelector('[data-cap-fill]');if(fill)fill.style.width=Math.max(0,Math.min(100,n*10))+'%';$$('[data-vm-count],[data-manage-count]').forEach(e=>e.textContent=n);}
function voiceRow(v){const row=document.createElement('div');row.className='vm-row'+(v.current?' on':'');const ava=document.createElement('span');ava.className='vm-row-avatar';ava.setAttribute('aria-hidden','true');ava.textContent=(v.name||'?').slice(0,1);const copy=document.createElement('div');copy.className='vm-row-copy';const name=document.createElement('b');name.textContent=v.name;const sub=document.createElement('small');sub.textContent=v.current?'正在使用':'我的音色';copy.append(name,sub);const acts=document.createElement('div');acts.className='vm-row-actions';const mk=(cls,label,act,icon)=>{const b=document.createElement('button');b.type='button';b.className=cls;b.dataset.voiceId=v.id;b.dataset.voiceAction=act;if(icon){b.setAttribute('aria-label',label+' '+v.name);b.innerHTML='<svg class="ico" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><use href="#i-'+icon+'"></use></svg>';}else{b.textContent=label;}return b;};acts.append(mk('vm-use','使用','select'),mk('vm-icon','试听','audition','play'),mk('vm-icon','重命名','rename','edit'),mk('vm-icon','删除','delete','trash'));row.append(ava,copy,acts);return row;}
async function voices(quiet=false){
 if(voiceBusy||auditionPending||document.hidden)return;
 clearTimeout(voiceRetry);
 voiceBusy=true;
 try{
  const d=await api('/api/user/voices');
  if(d.voice_management_busy)voiceRetry=setTimeout(()=>voices(quiet),1500);
  if(!Array.isArray(d.voices)||d.voices.some(v=>!v||typeof v.id!=='string'||typeof v.name!=='string'))return;
  voiceAvailable=d.has_key===false?false:true;
  paintVoiceService();
  catalogLoaded=true;
  const signature=JSON.stringify(d.voices);
  if(signature===voiceSignature)return;
  voiceSignature=signature;
  catalog=d.voices;current=catalog.find(v=>v.current)||null;const label=current?.name|| (d.has_key===false?'语音服务未配置':'暂无音色');$$('.voice-now,.voice-meta b,[data-vm-current]').forEach(e=>textIfChanged(e,label));
 const fixed=$('.fixed-track'), rail=$('.clone-rail'), list=$('[data-vm-list]');fixed.textContent='';rail.querySelectorAll('.clone-chip').forEach(e=>e.remove());list.textContent='';
 for(const v of catalog){const chip=document.createElement('button');chip.type='button';chip.className='voice-chip'+(v.builtin?'':' clone-chip')+(v.current?' on':'');chip.textContent=v.name;chip.dataset.voiceId=v.id;if(v.builtin)fixed.append(chip);else{rail.insertBefore(chip,rail.querySelector('[data-voice-add]'));list.append(voiceRow(v));}}
 const n=catalog.filter(v=>!v.builtin).length;paintCapacity(n);if(!quiet&&d.has_key===false&&!d.voice_management_busy)toast('外部语音服务未配置，可使用原声');
 }catch(e){if(!loginFor(e)&&!quiet)toast(e.message);}
 finally{voiceBusy=false;}
}
async function watchAudition(){
 for(let i=0;i<45;i++){
  await sleep(1000);
  const {phase}=await api('/api/user/voices/audition/status');
  if(phase==='playing')$('.now-voice')?.classList.add('auditioning');
  if(phase==='complete'){toast('试听结束');return;}
  if(phase==='cancelled'){toast('试听已取消');return;}
  if(phase==='failed')throw new Error('试听失败，请检查网络和语音服务');
 }
 toast('暂未收到试听结果');
}
async function audition(id){
 if(id==='xiaozhi-native'){toast('原声会在正常对话中播放');return;}
 if(auditionPending){toast('试听进行中，请稍候');return;}
 auditionPending=true;
 try{await api('/api/user/voices/audition',{id});}
 catch(e){auditionPending=false;throw e;}
 toast('正在准备试听');
 // Keep the conversation controls usable while this result is pending.
 watchAudition().catch(e=>{if(!loginFor(e))toast(e.message);}).finally(()=>{
  auditionPending=false;
  $('.now-voice')?.classList.remove('auditioning');
 });
}
async function action(act,id){
 if(act==='audition')return audition(id);
 const editing=act==='rename'||act==='delete';
 const name=editing?await WzhVoiceActions.confirmAction(act,catalog.find(v=>v.id===id)?.name||'该音色'):'';
 if(name===null)return;
 const label=act==='rename'?'保存名称':act==='delete'?'删除音色':'切换音色';
 const notice=$('[data-voice-operation-status]');
 const native=act==='select'&&id==='xiaozhi-native';
 textIfChanged(notice,`正在${label}…`);
 try{
  const result=await api('/api/user/voices/'+(native?'native':act),{id,name});
  if(!native)await WzhVoiceActions.waitForResult(api,'/api/user/voices/manage/status',result.request);
  refreshCatalog=true;
  await voices(true);
  textIfChanged(notice,`${label}成功`);
  toast(`${label}成功`);
 }catch(error){
  textIfChanged(notice,error.message);
  throw error;
 }
}
function voiceFailure(reason){
 return ({
  short:'有效人声不足 5 秒，请在安静处靠近设备重录。', config:'音色服务尚未配置。',
  auth:'音色服务账户授权异常，请联系管理员。',limit:'音色服务额度不足，请联系管理员。',
  voice_full:'音色已满，请先删除一个已有音色。',full:'音色已满，请先删除一个已有音色。',
  memory:'设备内存暂时不足，请稍后重试。',network:'音色服务连接失败，请检查网络后重试。',
  busy:'设备正忙，请稍后重试。', uncertain:'请求结果尚未确认，请先查看音色列表再重试。',
  save:'音色记录保存失败，请稍后重试。',prompt:'设备未能播放录音提示，请检查网络后重试。'
 })[reason]||'创建失败，请检查语音服务后重试。';
}
function paintVoiceJob(message=''){
 const job=voiceJob,recording=job?.kind==='record'&&job?.phase==='recording';
 const cancel=$('[data-cancel-audio]'),record=$('[data-record-puck]'),create=$('[data-create-audio]');
 if(message)textIfChanged($('[data-voice-operation]'),message);
 record.disabled=!!job;
 record.classList.toggle('is-recording',recording);
 if(job)create.disabled=true;
 else create.disabled=!$('[data-upload-panel]').hidden&&!$('#audio-file-input').files.length;
 textIfChanged(create,$('[data-upload-panel]').hidden?'开始设备录音':'上传并生成');
 cancel.disabled=!!job?.cancelRequested;
 textIfChanged(cancel,job?.kind==='record'?
  (job.phase==='processing'?'取消生成':'停止录音'):job?'关闭':'取消');
 if(recording){
  const elapsed=Math.min(20,Math.floor((Date.now()-job.recordedAt)/1000));
  textIfChanged($('[data-record-time]'),'00:'+String(elapsed).padStart(2,'0'));
  $('[data-record-progress]').style.width=elapsed*5+'%';
 }
}
async function cancelRecording(job){
 job.cancelRequested=true;
 paintVoiceJob('正在停止设备录音…');
 if(!job.request||job.cancelSent)return;
 job.cancelSent=true;
 try{await api('/api/user/voices/device-record/cancel',{request:job.request});}
 catch(e){
  job.cancelRequested=job.cancelSent=false;
  paintVoiceJob(e.message);
  if(!loginFor(e))toast(e.message);
 }
}
async function watchVoiceJob(job,path){
 const deadline=Date.now()+180000;
 let interrupted=false;
 while(Date.now()<deadline){
  await sleep(1000);
  let s;
  try{s=await api(path);}
  catch(e){
   if(e.status===401)throw e;
   if(!interrupted)paintVoiceJob('连接暂时中断，正在恢复进度…');
   interrupted=true;
   continue;
  }
  interrupted=false;
  if(job.request && s.request && s.request!==job.request)
   throw new Error('已有其他录音任务，请查看音色列表。');
  if(s.phase==='success'||s.phase==='done')return '创建成功，已应用新音色。';
  if(s.phase==='cancelled')return '录音已停止。';
  if(s.phase==='failed')throw new Error(voiceFailure(s.failure||s.voice_management_status));
  if(s.phase==='recording'&&!job.recordedAt)job.recordedAt=Date.now();
  job.phase=s.phase;
  if(!job.cancelRequested)
   paintVoiceJob(({starting:'正在准备录音…',prompt:'请听设备提示，靠近设备说话。',
    recording:'设备正在录音，最多 20 秒。',processing:'正在生成音色…',working:'正在生成音色…'})[s.phase]);
 }
 return '仍未收到最终结果，请查看音色列表后再试。';
}
async function record(job,name){
 paintVoiceJob('正在启动设备录音…');
 $('[data-record-time]').textContent='00:00';
 $('[data-record-progress]').style.width='0%';
 const accepted=await api('/api/user/voices/device-record',{name});
 job.request=accepted.request;
 if(job.cancelRequested)await cancelRecording(job);
 return watchVoiceJob(job,'/api/user/voices/device-record/status');
}
async function upload(job,name){
 const file=$('#audio-file-input').files[0];
 if(!file)throw new Error('请选择录音');
 if(file.size>20*1024*1024)throw new Error('文件超过 20 MB');
 const Audio=window.AudioContext||window.webkitAudioContext;
 const Offline=window.OfflineAudioContext||window.webkitOfflineAudioContext;
 if(!Audio||!Offline)throw new Error('浏览器不支持音频处理');
 paintVoiceJob('正在处理手机里的音频…');
 const audio=new Audio();
 let decoded;
 try{decoded=await audio.decodeAudioData(await file.arrayBuffer());}
 finally{await audio.close();}
 if(decoded.duration<5||decoded.duration>20.05)throw new Error('录音需要 5–20 秒');
 const render=new Offline(1,Math.min(320000,Math.round(decoded.duration*16000)),16000);
 const source=render.createBufferSource();
 source.buffer=decoded;source.connect(render.destination);source.start();
 const samples=(await render.startRendering()).getChannelData(0);
 const pcm=new ArrayBuffer(samples.length*2),view=new DataView(pcm);
 samples.forEach((v,i)=>view.setInt16(i*2,Math.round(Math.max(-1,Math.min(1,v))*32767),true));
 paintVoiceJob('正在上传录音，请保持手机连接设备热点…');
 await api('/api/user/voices/clone',undefined,{method:'POST',timeout:30000,
  headers:{'Content-Type':'application/octet-stream','X-Voice-Name':encodeURIComponent(name)},body:pcm});
 return watchVoiceJob(job,'/api/user/voices/clone/status');
}
async function createVoice(kind){
 if(voiceJob){toast('正在处理上一段录音');return;}
 const name=$('[data-audio-name-input]').value.trim();
 if(!name){paintVoiceJob('请先输入音色名称。');return;}
 const job=voiceJob={kind};
 try{
  const message=await (kind==='upload'?upload(job,name):record(job,name));
  paintVoiceJob(message);toast(message);
  refreshCatalog=true;
  voices(true);
 }catch(e){
  if(!loginFor(e)){paintVoiceJob(e.message);toast(e.message);}
 }finally{
  voiceJob=null;
  paintVoiceJob();
 }
}
async function reset(){
 const input=$('[data-reset-input]'),button=$('[data-reset-confirm]'),status=$('[data-reset-status]');
 const confirm=input.value.trim();
 if(confirm!=='确定恢复出厂设置')return;
 const phases={checking:'正在检查设备和音色记录',clearing_local:'正在清理本地音色和教程进度',
  clearing_wifi:'正在清除 Wi-Fi 凭据',rebooting:'设备正在重启，请重新连接设备热点',failed:'恢复失败，请重试'};
 let accepted=false;
 input.disabled=button.disabled=true;
 status.textContent='正在启动恢复任务，请保持设备供电';
 try{
  await api('/api/user/factory-reset',{confirm});
  accepted=true;
  const deadline=Date.now()+240000;
  for(;Date.now()<deadline;){
   await sleep(1000);
   let s;
   try{s=await api('/api/user/factory-reset/status');}
   catch(e){
    if(e.status===401)throw e;
    textIfChanged(status,'设备连接暂时中断，正在继续查询恢复结果…');
    continue;
   }
   let message=s.error?.message||phases[s.phase]||'正在恢复';
   if(s.phase==='deleting_cloud')message=s.total>0?`正在删除云端音色（${s.current}/${s.total}）`:'正在检查云端音色';
   if(s.phase==='failed'&&s.current>0)message+=` 已删除 ${s.current}/${s.total} 个云端音色。`;
   if(s.phase==='idle'){message='设备已重新启动，请返回首页查看配网状态。';}
   textIfChanged(status,message);
   if(s.phase==='failed'){
    toast(message);
    return;
   }
   if(s.phase==='rebooting'||s.phase==='idle'){
    input.value='';
    toast(message);
    return;
   }
  }
  textIfChanged(status,'暂未收到恢复结果，请确认手机仍连接设备热点。');
 }catch(error){
  if(loginFor(error))return;
  if(!accepted){status.textContent=error.message;throw error;}
  status.textContent='设备连接中断，请重新连接设备热点查看结果';
  toast(status.textContent);
 }finally{
  input.disabled=false;
  button.disabled=input.value.trim()!=='确定恢复出厂设置';
 }
}
async function chat(){
 if(chatBusy||!lastStatus)return;
 const stop=lastStatus.conversation_recovering||['connecting','listening','speaking'].includes(lastStatus.state);
 chatBusy=true;
 paintChat(lastStatus);
 try{
  await api(stop?'/api/user/chat/stop':'/api/user/chat/start',{});
  statusPending=statusBusy;
  await status();
  setTimeout(status,650);
 }catch(e){if(!loginFor(e))toast(e.message);}
 finally{chatBusy=false;paintChat(lastStatus);}
}
document.addEventListener('click',async e=>{
 if(e.target.closest('.lab-record,.lab-upload,[data-voice-add],[data-vm-new],[data-create-tab]')){
  if(voiceAvailable===false||(lastStatus&&!networkOnline(lastStatus))){
   e.preventDefault();e.stopImmediatePropagation();
   toast($('#voiceServiceNotice').textContent);
   return;
  }
  setTimeout(()=>{
   paintVoiceJob();
   textIfChanged($('[data-record-status]'),'使用设备麦克风，开始前请听提示。');
  },0);
  return;
 }
 const target=e.target.closest('button');if(!target)return;
 if(target.matches('.sheet-main > .primary,.talk-cta,.float-assistant')){
  e.preventDefault();e.stopImmediatePropagation();
  await chat();
  return;
 }
 if(target.matches('[data-record-puck],[data-create-audio]')){
  e.preventDefault();e.stopImmediatePropagation();
  await createVoice($('[data-upload-panel]').hidden?'record':'upload');
  return;
 }
 if(target.matches('[data-cancel-audio]')&&voiceJob?.kind==='record'){
  e.preventDefault();e.stopImmediatePropagation();
  await cancelRecording(voiceJob);
  return;
 }
 let task=null;
 if(target.matches('.manual-ribbon'))task=()=>location.href='/manual';
 else if(target.matches('.net-change'))task=()=>{location.href='/setup?from=device';};
 else if(target.matches('.voice-try,.listen-mini,[data-vm-audition]'))task=()=>current?action('audition',current.id):Promise.reject(new Error('没有可试听的音色'));
 else if(target.dataset.voiceId)task=()=>action(target.dataset.voiceAction||'select',target.dataset.voiceId);
 else if(target.matches('[data-reset-confirm]'))task=reset;
 if(!task)return;e.preventDefault();e.stopImmediatePropagation();if(working){toast('设备正在处理，请稍候');return;}working=true;try{await task();}catch(err){if(!loginFor(err))toast(err.message||'操作失败');}finally{working=false;}
},true);
document.addEventListener('change',e=>{
 if(e.target.matches('#audio-file-input')){
  const file=e.target.files[0],panel=$('[data-upload-panel]');
  $('[data-audio-file]').textContent=file?.name||'点击选择音频文件';
  panel.classList.toggle('has-file',!!file);
  if(!panel.hidden)$('[data-create-audio]').disabled=!file||!!voiceJob;
  const name=$('[data-audio-name-input]');
  if(file&&(!name.value||name.value==='我的音色')){
   name.value=file.name.replace(/\.[^.]+$/,'').slice(0,8)||'我的音色';
  }
 }else if(e.target.matches('.volume')){
  setVolume(Number(e.target.value));
 }
},true);
document.addEventListener('DOMContentLoaded',()=>{
  // Clear prototype sample values before reading the device. Never expose demo data as telemetry.
  $$('.metrics .metric b,.battery-core b,.readout-copy b,.net-chip,.net-name,.net-val,[data-metric-net],.voice-now,.voice-meta b,[data-vm-current]').forEach(e=>e.textContent='--');
  $$('.clone-chip').forEach(e=>e.remove());
  const list=$('[data-vm-list]'); if(list) list.textContent='';
  const track=$('.fixed-track'); if(track) track.textContent='';
  const trim=$('.audio-trim'); if(trim) trim.hidden=true; // The upload API accepts the complete 5–20 s clip.
  textIfChanged($('.status-text'),'正在连接设备…');
 paintChat(null);
  status();
  window.addEventListener('pageshow',event=>{if(event.persisted)resumeStatus();});
 document.addEventListener('visibilitychange',()=>{
  if(document.hidden){clearTimeout(statusTimer);clearTimeout(voiceRetry);}
  else resumeStatus();
 });
});
// Dinosaur only: runtime 4G/Wi-Fi transport switch, rendered from /api/status.
window.__renderTransportSwitch=function(s){
  const box=$('#transportSwitch');if(!box)return;
  if(!s||(!s.probing&&!s.switchable)){box.hidden=true;return;}
  box.hidden=false;
  const hint=box.querySelector('.ts-hint');if(hint)hint.textContent=s.probing?'正在切换网络，请稍候':'切换会断开当前连接并重新联网';
  const btns=box.querySelectorAll('.ts-btn');
  btns.forEach(b=>{
    const isCell=b.dataset.transport==='cellular';
    b.classList.toggle('on',!s.probing&&isCell===s.cell);
    b.disabled=s.probing||isCell===s.cell;
    b.onclick=async()=>{
      if(working){toast('设备正在处理，请稍候');return;}
      working=true;btns.forEach(x=>x.disabled=true);
      try{await api('/api/network/transport',{transport:b.dataset.transport});toast(s.cell?'正在切换到 Wi-Fi…':'正在切换到 4G…');}
      catch(e){
        if(loginFor(e))return;
        if(e.timeout||!e.status)toast('网络切换中，请稍候…');
        else toast(e.message);
      }
      finally{working=false;setTimeout(status,1500);setTimeout(status,6500);}
    };
  });
};
})();
