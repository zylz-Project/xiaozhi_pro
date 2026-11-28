(function(){
'use strict';
const $=id=>document.getElementById(id), q=s=>document.querySelector(s), qa=s=>Array.from(document.querySelectorAll(s));
let statusData={}, audioData={}, filter='all', token='';
let statusJob = null, touchPollTimer = null, touchBuild = '', sessionTimer = null;
let voicePreviewPending = false;
let hubPollTimer = null;
let toastTimer = null, audioJob = null, voiceCatalogSignature = '';
const touchPoints = qa('[data-touch-sw]');
const sw4Isolation = $('toggleSw4Isolation');
const touchLocations = new Map([
  ['wzh-crawlpanda', ['头', '肚子', '背部前侧', '背部后侧']],
  ['wzh-tailredpanda', ['头', '背部', '肚子', '尾巴']],
  ['wzh-dinosaur', ['头', '胸前', '背部靠近脖子', '背部靠近尾巴']]
]);
function toast(message){
  clearTimeout(toastTimer);
  setText('devToast',message);
  $('devToast').classList.add('show');
  toastTimer=setTimeout(()=>$('devToast').classList.remove('show'),3000);
}
async function api(path,body,options={}){
  const o={credentials:'same-origin',cache:'no-store',...options};
  const session = token;
  o.headers={...(session?{'X-Developer-Token':session}:{}),...(o.headers||{})};
  if(body!==undefined){o.method='POST';o.headers['Content-Type']='application/json';o.body=JSON.stringify(body);}
  let timer;
  if(!o.signal){
    const controller=new AbortController();
    o.signal=controller.signal;
    timer=setTimeout(()=>controller.abort(),15000);
  }
  try{
    const r=await fetch(path,o),d=await r.json().catch(()=>null);
    if(!r.ok||d?.ok===false){
      const reason = typeof d?.error === 'string' ? d.error : d?.error?.message;
      if (session && token === session && (r.status === 401 || reason === 'developer_locked')) {
        gate('开发者会话已结束，请重新登录');
      }
      const errors = { invalid_password:'密码不正确', rate_limited:'尝试次数过多，请在 60 秒后重试',
        developer_locked:'请重新登录', voice_preview_native:'原声会在正常对话中播放',
        voice_preview_no_key:'语音服务未配置', voice_preview_busy:'设备正忙，请稍后试听',
        touch_isolation_save_failed:'保存 SW4 设置失败，请重试' };
      throw Error(errors[reason] || reason || '请求失败 ('+r.status+')');
    }
    if(!d||typeof d!=='object'||Array.isArray(d))
      throw Error('设备回复不完整，请重试');
    return d;
  }catch(e){
    if(timer&&e.name==='AbortError')throw Error('设备响应超时，请检查连接后重试');
    throw e;
  }finally{clearTimeout(timer);}
}
function gate(message = ''){
  clearTimeout(sessionTimer);
  $('devPhone').dataset.state='gate';
  token='';
  touchBuild='';
  statusData={};
  audioData={};
  voiceCatalogSignature='';
  setText('gateError', message);
  renderTouch('', null, '等待读取');
  renderAccelerometer(null, '等待读取');
  scheduleTouchPoll();
}
function updateText(node, value) {
  const text = String(value ?? '--');
  if (node && node.textContent !== text) node.textContent = text;
}
function setText(id,v){updateText($(id),v)}
function rowValues(panel,card){return qa(`[data-panel="${panel}"] .d-card`)[card]?.querySelectorAll('.d-row .d-value')||[]}
function values(nodes,items){nodes.forEach((n,i)=>updateText(n,items[i]))}
function download(name,data){let a=document.createElement('a');a.href=URL.createObjectURL(new Blob([data],{type:'application/json'}));a.download=name;a.click();setTimeout(()=>URL.revokeObjectURL(a.href),1000)}
function isTouchCode(code) {
  return Number.isInteger(code) && code >= 0 && code < 16;
}
function renderSw4Isolation(isolated) {
  if (!sw4Isolation) return;
  sw4Isolation.hidden = typeof isolated !== 'boolean';
  sw4Isolation.dataset.isolated = isolated === true ? '1' : '0';
  updateText(sw4Isolation, isolated ? '恢复 SW4 触摸' : '隔离 SW4 触摸');
}
function renderTouch(build, touch, unavailable = '不可用') {
  renderSw4Isolation(touch?.sw4_isolated);
  const labels = touchLocations.get(build) || [];
  const available = touch?.available === true;
  const stable = available && touch.valid === true && isTouchCode(touch.code);
  const pending = available && !stable && isTouchCode(touch.raw_code);
  let unknown = unavailable;
  if (available) unknown = pending ? '等待稳定' : '读数无效';
  let baseState = stable ? 'released' : 'unknown';
  if (pending) baseState = 'pending';
  const baseLabel = stable ? '已松开' : unknown;
  let pressedCount = 0;
  touchPoints.forEach((point, index) => {
    const isolated = index === 3 && touch?.sw4_isolated === true;
    const pressed = stable && Boolean(touch.code & (1 << index));
    if (pressed) pressedCount++;
    const state = isolated ? 'isolated' : pressed ? 'pressed' : baseState;
    if (point.dataset.state !== state) point.dataset.state = state;
    updateText(point.querySelector('[data-touch-location]'), labels[index] || '');
    updateText(point.querySelector('[data-touch-state]'), isolated ? '已隔离' : pressed ? '已触摸' : baseLabel);
  });
  const summary = stable ? '全部松开' : unknown;
  const summaryText = pressedCount ? `${pressedCount} 路触发` : summary;
  setText('touchSummary', summaryText);
  setText('touchChipValue', summaryText);
  const chip = $('touchChip');
  chip.classList.toggle('ok', stable);
  chip.classList.toggle('warn', available && !stable);
  chip.classList.toggle('off', !available);
  const mv = touch?.voltage_mv;
  const voltage = available && Number.isFinite(mv) && mv >= 0 ? `${mv} mV` : '--';
  const formatCode = code => `0x${code.toString(16).toUpperCase().padStart(2, '0')}`;
  const code = stable ? formatCode(touch.code) : '--';
  const raw = available && isTouchCode(touch.raw_code) ? formatCode(touch.raw_code) : '--';
  setText('touchDiagnostics', `IO5 ${voltage} · 稳定 ${code} · 原始 ${raw}`);
}
const stateNames = {
  idle:'待命', connecting:'连接中', listening:'聆听中', speaking:'播报中', starting:'启动中',
  wifi_configuring:'配网中', activating:'连接服务中', upgrading:'升级中', audio_testing:'音频测试', fatal_error:'设备异常'
};
function reading(value, unit) { return Number.isFinite(value) ? `${value}${unit}` : '--'; }
function renderAccelerometer(sensor, unavailable = '不可用') {
  const axes = [sensor?.x_mg, sensor?.y_mg, sensor?.z_mg];
  const valid = sensor?.accelerometer_valid === true && axes.every(Number.isFinite);
  const anglesValid = valid && sensor.angles_valid === true;
  setText('accelSummary', valid ? '采样有效' : (sensor ? '读数无效' : unavailable));
  setText('accelAxes', valid ? `X ${axes[0]} / Y ${axes[1]} / Z ${axes[2]} mg` : '--');
  setText('accelMagnitude', valid ? reading(Math.round(Math.hypot(...axes)), ' mg') : '--');
  const orientation = ['+X', '−X', '+Y', '−Y', '+Z', '−Z'][sensor?.orientation];
  setText('accelOrientation', anglesValid ? (sensor.orientation === -1 ? '倾斜' : orientation) : '--');
  const angle = value => Number.isFinite(value) ? `${value.toFixed(1)}°` : '--';
  setText('accelAngles', anglesValid ? `${angle(sensor.pitch)} / ${angle(sensor.roll)}` : '--');
}
function byteSize(value) {
  if (!Number.isFinite(value)) return '--';
  if (value >= 1048576) return `${(value / 1048576).toFixed(1)} MiB`;
  if (value >= 1024) return `${(value / 1024).toFixed(1)} KiB`;
  return `${value} B`;
}
function onOff(value, on, off) { return typeof value === 'boolean' ? (value ? on : off) : '--'; }
function renderChip(id, enabled, on, off) {
  const chip = $(id);
  chip.classList.toggle('ok', enabled === true);
  chip.classList.toggle('off', enabled !== true);
  updateText(chip.querySelector('b'), onOff(enabled, on, off));
}
function renderRuntime(s) {
  const w = s.wifi || {}, b = s.battery || {}, a = s.audio || {}, f = s.flash || {};
  const cellular = s.transport === 'cellular';
  const connected = typeof s.network_available === 'boolean' ? s.network_available : w.connected;
  const server = onOff(s.cellular_server_connected, '对话服务已连接', '对话服务未连接');
  const wifiSignal = w.connected ? reading(w.rssi, ' dBm') : '未连接';
  setText('runtimeState', stateNames[s.state] || s.state);
  setText('runtimeSub', s.maintenance ? '维护中' : onOff(s.conversation, '对话模式', '离线互动'));
  setText('uptime', Number.isFinite(s.uptime_s) ? `${Math.floor(s.uptime_s / 60)} 分 ${Math.floor(s.uptime_s % 60)} 秒` : '--');
  setText('batteryLevel', b.available ? reading(b.level, '%') : '--');
  const hasRest = Number.isFinite(b.voltage_mv) && b.voltage_mv > 0;
  setText('batteryRest', hasRest ? reading(b.voltage_mv, ' mV') : '等待静置采样');
  setText('chipTemp', Number.isFinite(s.temperature_c) ? `${s.temperature_c.toFixed(1)} °C` : '--');
  const probing = s.transport === 'probing';
  setText('networkLink', probing ? '检测中' : (connected ? (cellular ? '4G 已联网' : 'Wi-Fi 已联网') : '未联网'));
  setText('networkDetail', probing ? '正在选择联网通道' : (cellular ? server : wifiSignal));
  renderChip('micChip', a.input_enabled, '已启用', '已关闭');
  renderChip('speakerChip', a.speaker_enabled, '已启用', '已关闭');
  renderChip('servoChip', s.servo_powered, '已上电', '已断电');
  setText('batteryVoltages', `${reading(b.live_voltage_mv, ' mV')} / ${reading(b.protection_voltage_mv, ' mV')}`);
  const sample = onOff(b.sample_valid, b.under_load ? '负载中' : '空载', '无效');
  const restAge = hasRest && Number.isFinite(b.rest_age_ms) && b.rest_age_ms >= 0 ? `${Math.floor(b.rest_age_ms / 1000)} 秒前` : '待采样';
  setText('batterySample', `${sample} / ${restAge}`);
  setText('servoFaults', Number.isInteger(s.servo_faults) ? `0x${s.servo_faults.toString(16).toUpperCase()}` : '--');
  setText('captureState', `${onOff(a.capture_running, '采集中', '已停止')} / ${onOff(a.wake_running, '监听中', '已停止')}`);
  setText('playbackState', `${onOff(a.playback_idle, '空闲', '活跃')} / ${reading(a.buffered_ms, ' ms')}`);
  setText('audioStorage', f.type ? `${f.type} · ${reading(f.files, ' 个')}${f.writing ? ' · 写入中' : ''}` : '--');
}
function renderStatus(s) {
  statusData = s;
  if(window.PandaProduct)PandaProduct.set(s.build==='wzh-dinosaur'?'dinosaur':s.build==='wzh-tailredpanda'?'redpanda':'crawlPanda',false);
  const w = s.wifi || {}, a = s.audio || {}, f = s.flash || {};
  const cellular = s.transport === 'cellular';
  const transport = cellular ? '4G' : 'Wi-Fi';
  const link = onOff(s.network_available ?? w.connected, '已连接', '未连接');
  setText('netTransport', s.transport === 'probing' ? '正在选择联网通道' : `${transport} · ${link}`);
  $('netServerRow').hidden = !cellular;
  setText('netServer', onOff(s.cellular_server_connected, '已连接', '未连接'));
  setText('netWifi', w.connected ? w.ssid || '已连接' : '未连接');
  setText('netIp', w.connected && w.ip ? w.ip : '--');
  setText('netRssi', w.connected ? `${reading(w.channel, ' 信道')} / ${reading(w.rssi, ' dBm')}` : '--');
  setText('netDevAddress', location.origin);
  values(rowValues('system',0), [s.version, [s.compiled, s.compiled_time].filter(Boolean).join(' ') || '--',
    s.idf_version, s.chip, `${byteSize(s.flash_bytes)} / ${byteSize(s.psram_bytes)}`, s.sta_mac, s.build,
    `${byteSize(s.heap_internal)} / ${byteSize(s.heap_internal_min)}`, byteSize(s.heap_internal_largest)]);
  values(rowValues('system',3), [s.app_partition ? `${s.app_partition} · ${byteSize(s.app_partition_bytes)}` : '--',
    `${byteSize(f.used)} / ${byteSize(f.capacity)}`, `${s.nvs_used_entries ?? '--'} / ${s.nvs_total_entries ?? '--'}`]);
  setText('audioStorageSummary', f.type ? `${reading(f.files, ' 个')} · ${byteSize(f.used)}` : '--');
  $('devVolume').value=a.volume??0;
  setText('devVolVal',a.volume);
  const selected = s.ali_voice_id;
  setText('devVoiceName',typeof selected==='string'&&(!selected||selected==='xiaozhi-native')?'原声':selected||'未配置');
  setText('devPrevVoice',s.ali_previous_voice_id==='xiaozhi-native'?'原声':s.ali_previous_voice_id||'无');
  $('devVoiceRestore').disabled=!s.ali_previous_voice_id;
  setText('createStatus',s.voice_enrollment_status||'未开始');
  const voices=s.voice_catalog||[];
  const signature=JSON.stringify(voices);
  if(signature!==voiceCatalogSignature){
    voiceCatalogSignature=signature;
    renderVoices(voices);
  }
}
function refresh(liveOnly = false) {
  if (!token) return Promise.resolve();
  if (statusJob?.session === token) {
    if (liveOnly || !statusJob.liveOnly) return statusJob.promise;
    return statusJob.promise.catch(() => {}).then(() => refresh());
  }
  const session = token;
  const promise = api(liveOnly ? '/api/status' : '/api/developer/status').then(s => {
    if (token !== session) return;
    touchBuild = s.build;
    renderTouch(touchBuild, s.touch);
    renderAccelerometer(s.sensors);
    renderRuntime(s);
    if (!liveOnly) renderStatus(s);
  }).catch(error => {
    if (token === session) {
      renderTouch(touchBuild, null, '读取失败');
      renderAccelerometer(null, '读取失败');
      setText('runtimeSub', '读取失败，显示上次状态');
    }
    throw error;
  }).finally(() => { if (statusJob?.promise === promise) statusJob = null; });
  statusJob = {liveOnly, promise, session};
  return promise;
}
function canPollTouch() {
  return token && !document.hidden && !q('[data-panel="status"]').hidden;
}
function scheduleTouchPoll(delay = 1000) {
  clearTimeout(touchPollTimer);
  if (!canPollTouch()) return;
  touchPollTimer = setTimeout(async () => {
    if (!canPollTouch()) return;
    try { await refresh(true); } catch (error) { /* The sensor cards show the read failure. */ }
    scheduleTouchPoll();
  }, delay);
}
document.addEventListener('visibilitychange', () => scheduleTouchPoll(0));
function button(label,fn){let b=document.createElement('button');b.type='button';b.className='d-mini';b.textContent=label;b.onclick=()=>run(fn);return b}
let voiceMutationPending=false;
async function manageVoice(voice, action) {
  if(voiceMutationPending){toast('音色操作正在进行');return;}
  voiceMutationPending=true;
  try{
    const name=action==='select'?'':await WzhVoiceActions.confirmAction(action,voice.name);
    if(name===null)return;
    const label=action==='rename'?'保存名称':action==='delete'?'删除音色':'切换音色';
    setText('createStatus','正在'+label+'…');
    const result=await api('/api/developer/voice-manage',{action,id:voice.id,name});
    await WzhVoiceActions.waitForResult(api,'/api/developer/status',result.request);
    await refresh();
    setText('createStatus',label+'成功');
    toast(label+'成功');
  }catch(error){
    setText('createStatus',error.message);
    throw error;
  }finally{voiceMutationPending=false;}
}
function renderVoices(voices) {
  const fixed=$('fixedVoiceChips'), clones=$('cloneList');
  fixed.replaceChildren();clones.replaceChildren();
  let count=0;
  for(const voice of voices){
    if(voice.builtin){
      const chip=button(voice.name,()=>config('ali_voice',voice.id));
      chip.className='d-voice-chip';fixed.append(chip);continue;
    }
    count++;
    const row=document.createElement('div');row.className='d-voice-row';
    const name=document.createElement('span');name.className='v-name';name.textContent=voice.name;
    row.append(name);
    for(const [label,action] of [['使用','select'],['改名','rename'],['删除','delete']])
      row.append(button(label,()=>manageVoice(voice,action)));
    clones.append(row);
  }
  setText('cloneCap',count+' / 10');
}
async function config(kind,value=''){await api('/api/developer/voice-config',{kind,value});toast('请求已提交');setTimeout(()=>run(refresh),1200)}
async function auditionVoice() {
  if (voicePreviewPending) return;
  const session = token;
  const buttons = ['devVoicePreview', 'devVoiceAudition'].map($);
  voicePreviewPending = true;
  buttons.forEach(button => button.disabled = true);
  try {
    await api('/api/developer/voice-config', {kind:'ali_preview', value:''});
    if (token !== session) return;
    toast('正在准备试听');
    for (let i = 0; i < 45; ++i) {
      await new Promise(resolve => setTimeout(resolve, 1000));
      if (token !== session) return;
      const {phase} = await api('/api/developer/voice-preview/status');
      if (token !== session) return;
      if (phase === 'complete') { toast('试听结束'); return; }
      if (phase === 'cancelled') { toast('试听已取消'); return; }
      if (phase === 'failed') throw Error('试听失败，请检查网络和语音服务');
    }
    toast('暂未收到试听结果');
  } finally {
    voicePreviewPending = false;
    buttons.forEach(button => button.disabled = false);
  }
}
function run(fn){Promise.resolve().then(fn).catch(e=>toast(e.message||'操作失败'))}
sw4Isolation?.addEventListener('click', () => run(async () => {
  sw4Isolation.disabled = true;
  try {
    const result = await api('/api/developer/touch/sw4', {isolated:sw4Isolation.dataset.isolated !== '1'});
    renderSw4Isolation(result.isolated);
    toast(result.isolated ? 'SW4 已隔离' : 'SW4 触摸已恢复');
    await refresh();
  } finally { sw4Isolation.disabled = false; }
}));
function audio(){
  const session=token;
  if(audioJob?.session===session)return audioJob.promise;
  const button=$('refreshAudio');
  button.disabled=true;
  const promise=api('/api/audio').then(data=>{
    if(token!==session)return;
    audioData=data;
    renderAudio();
  }).finally(()=>{
    if(audioJob?.promise!==promise)return;
    audioJob=null;
    button.disabled=false;
  });
  audioJob={session,promise};
  return promise;
}
function renderAudio(){let list=$('audioList');list.replaceChildren();let files=[...(audioData.paired||[]),...(audioData.files||[])], term=$('audioSearch').value.toLowerCase();let shown=0;files.forEach(f=>{if(filter!=='all'&&f.category!==filter||!f.name.toLowerCase().includes(term))return;shown++;let row=document.createElement('div');row.className='d-audio-row';let name=document.createElement('span');name.className='a-main';name.textContent=f.name;row.append(name,button('播放',()=>api('/api/audio/play',{index:f.index}).then(()=>toast('播放请求已提交'))),button('停止',()=>api('/api/audio/stop',{}).then(()=>toast('已停止'))));list.append(row)});let count=document.createElement('p');count.className='d-sub';count.textContent=`显示 ${shown} / ${files.length} 个`;list.append(count)}
async function hub() {
  clearTimeout(hubPollTimer);
  const session = token;
  const h = (await api('/api/hub/status')).hub || {};
  if (session !== token || activeTab !== 'hub') return;
  const binding = !h.available ? '不可用' : h.activated ? '已登记' : '未登记';
  values(rowValues('hub', 0), [h.device_id, '局域网', binding, statusData.flash?.files]);
  setText('hubChecked', h.job_running ? '处理中' : h.job_message ? '最近一次操作' : '尚未检查');
  setText('hubMsg', h.job_message || '勾选需要写入的音频。本机独有的录音会保留。');
  $('hubCheck').disabled = !h.available || h.job_running === true;
  $('hubUpdate').disabled = true;
  if (h.job_running) {
    hubPollTimer = setTimeout(() => {
      if (session === token && activeTab === 'hub') run(hub);
    }, 1500);
    return;
  }
  const diff = await api('/api/hub/diff');
  if (session !== token || activeTab !== 'hub') return;
  const card = $('hubDiffMeta').closest('.d-card');
  qa('[data-hub-item]').forEach(input => input.closest('label').remove());
  (diff.items || []).forEach(item => {
    const label = document.createElement('label');
    label.className = 'd-diff-row';
    const check = document.createElement('input');
    check.type = 'checkbox';
    check.dataset.hubItem = item.name;
    check.disabled = item.state === 'local';
    const text = document.createElement('span');
    text.textContent = item.name + ' · ' + item.state;
    label.append(check, text);
    card.insertBefore(label, card.querySelector('.d-progress'));
  });
  $('hubUpdate').disabled = !diff.ready || !(diff.items || []).some(item => item.state !== 'local');
  setText('hubDiffMeta', diff.ready ? (diff.items || []).length + ' 项' : '尚未检查');
}

async function startHubJob(path, body) {
  const session = token;
  const wasUpdateDisabled = $('hubUpdate').disabled;
  $('hubCheck').disabled = $('hubUpdate').disabled = true;
  try {
    await api(path, body);
    if (token === session && activeTab === 'hub') await hub();
  } catch (error) {
    if (token === session && activeTab === 'hub') {
      $('hubCheck').disabled = false;
      $('hubUpdate').disabled = wasUpdateDisabled;
    }
    throw error;
  }
}
async function loadTab(tab) {
  if (tab === 'audio') {
    await audio();
  } else if (tab === 'hub') {
    await hub();
  } else if (tab === 'system') {
    await refresh();
    const session = token;
    if (!session) return;
    const settings = await api('/api/developer/ota');
    if (token === session) $('otaUrl').value = settings.url || '';
  } else if (tab === 'voice') {
    await refresh();
    const session = token;
    if (session && statusData.ali_key_configured && !voicePreviewPending &&
        !['queued', 'playing'].includes(statusData.voice_preview_status)) {
      await api('/api/developer/voice-manage', {action:'list'});
      setTimeout(() => { if (token === session) run(refresh); }, 1800);
    }
  } else if (tab === 'network') {
    await refresh();
  }
}
const tabs = qa('.dev-tab');
let activeTab = 'status';
tabs.forEach(tab => tab.onclick = () => {
  clearTimeout(hubPollTimer);
  activeTab = tab.dataset.tab;
  tabs.forEach(t => t.setAttribute('aria-selected', t === tab ? 'true' : 'false'));
  qa('.dev-panel').forEach(panel => panel.hidden = panel.dataset.panel !== activeTab);
  $('devScroll').scrollTop = 0;
  scheduleTouchPoll(0);
  run(() => loadTab(activeTab));
});
let loginBusy = false;
$('devLogin').onclick = async () => {
  if (loginBusy) return;
  const input = $('devPassword'), button = $('devLogin'), error = $('gateError');
  if (!input.value) {
    setText('gateError', '请输入开发者密码');
    return;
  }
  loginBusy = true;
  button.disabled = true;
  setText('gateError', '正在验证，请稍候…');
  input.blur();
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), 15000);
  try {
    const result = await api('/api/developer/login', {password:input.value}, {signal:controller.signal});
    if (!result.token) throw Error('设备登录响应无效，请重试');
    token = result.token;
    input.value = '';
    input.type = 'password';
    $('devPhone').dataset.state = 'in';
    error.textContent = '';
    const lifetime = Number.isFinite(result.expires_in_s) ? result.expires_in_s * 1000 : 1800000;
    const expiry = new Date(Date.now() + lifetime);
    setText('sessTime', `到期 ${expiry.toLocaleTimeString([], {hour:'2-digit', minute:'2-digit', hour12:false})}`);
    sessionTimer = setTimeout(() => gate('开发者会话已结束，请重新登录'), lifetime);
    scheduleTouchPoll();
    run(async () => { await refresh(); if (activeTab !== 'status') await loadTab(activeTab); });
  } catch (error) {
    setText('gateError', error.name === 'AbortError' ? '连接超时，请检查网络后重试' :
      error instanceof TypeError ? '连接失败，请检查网络后重试' : error.message);
  } finally {
    clearTimeout(timer);
    loginBusy = false;
    button.disabled = false;
  }
};
$('devPassword').onkeydown = event => {
  if (event.key === 'Enter') { event.preventDefault(); $('devLogin').click(); }
};
$('devPwEye').onclick = () => $('devPassword').type = $('devPassword').type === 'password' ? 'text' : 'password';
['lockBtn', 'sessLock'].forEach(id => $(id).onclick = () => run(async () => {
  await api('/api/developer/logout', {});
  gate();
}));
$('refreshStatus').onclick = $('netDiag').onclick = () => run(refresh);
$('toggleChat').onclick = () => run(async () => { await api('/api/chat/toggle', {}); await refresh(); });
$('centerServo').onclick = () => run(() => api('/api/developer/center', {}).then(() => toast('回中请求已提交')));
['exportDiag', 'exportDiag2'].forEach(id => $(id).onclick = () => run(async () => {
  await refresh();
  if (token) download('device-diagnostics.json', JSON.stringify({status:statusData, audio:audioData}, null, 2));
}));
$('refreshAudio').onclick=()=>run(audio);$('audioSearch').oninput=renderAudio;qa('.d-filter').forEach(b=>b.onclick=()=>{filter=b.dataset.filter;qa('.d-filter').forEach(x=>x.classList.toggle('on',x===b));renderAudio()});$('exportAudio').onclick=()=>download('audio-index.json',JSON.stringify(audioData,null,2));
$('hubCheck').onclick = () => run(() => startHubJob('/api/hub/check', {}));
$('hubDiffBtn').onclick = () => run(hub);
$('hubKeep').onclick = () => toast('本机独有的录音会保留');
$('hubUpdate').onclick = () => run(() => {
  const names = qa('[data-hub-item]:checked:not(:disabled)').map(input => input.dataset.hubItem);
  if (!names.length) throw Error('请先选择音频');
  return startHubJob('/api/hub/update', {names});
});
$('devVolume').oninput=()=>setText('devVolVal',$('devVolume').value);$('devVolumeSave').onclick=()=>run(async()=>{await api('/api/developer/volume',{volume:Number($('devVolume').value)});toast('音量已保存')});$('aliKeyEye').onclick=()=>$('aliKey').type=$('aliKey').type==='password'?'text':'password';$('aliKeySave').onclick=()=>run(()=>config('ali_key',$('aliKey').value));$('devVoicePreview').onclick=$('devVoiceAudition').onclick=()=>run(auditionVoice);$('devVoiceRestore').onclick=()=>run(()=>config('ali_restore'));$('cloneRecord').onclick=()=>run(()=>config('device_record',$('cloneName').value.trim()));$('cloneFile').onchange=()=>{$('cloneCreate').disabled=!$('cloneFile').files.length};$('cloneCreate').onclick=()=>run(async()=>{let name=$('cloneName').value.trim(),file=$('cloneFile').files[0];if(!name||!file)throw Error('请输入名称并选择音频');let ac=new (window.AudioContext||window.webkitAudioContext)(),buf;try{buf=await ac.decodeAudioData(await file.arrayBuffer())}finally{await ac.close()}if(buf.duration<5||buf.duration>20.05)throw Error('音频应为 5 至 20 秒');let ctx=new OfflineAudioContext(1,Math.min(320000,Math.round(buf.duration*16000)),16000),src=ctx.createBufferSource();src.buffer=buf;src.connect(ctx.destination);src.start();let samples=(await ctx.startRendering()).getChannelData(0),bytes=new TextEncoder().encode(name);if(bytes.length>255)throw Error('名称过长');let data=new Uint8Array(1+bytes.length+samples.length*2);data[0]=bytes.length;data.set(bytes,1);let view=new DataView(data.buffer);samples.forEach((x,i)=>view.setInt16(1+bytes.length+i*2,Math.round(Math.max(-1,Math.min(1,x))*32767),true));await api('/api/developer/voice-upload',undefined,{method:'POST',headers:{'Content-Type':'application/octet-stream'},body:data});toast('创建请求已提交')});
$('eraseInput').oninput=()=>$('eraseBtn').disabled=$('eraseInput').value!=='ERASE';$('eraseBtn').onclick=()=>run(async()=>{if(!confirm('确定擦除外置 Flash 音频？'))return;await api('/api/developer/flash/erase',{confirm:'ERASE'});toast('擦除任务已提交')});
['netReconnect','netScan','otaCheck','otaStart','restartDev'].forEach(id=>{let b=$(id);b.disabled=true;b.title='固件暂不支持';if((b.textContent||'').trim())b.textContent+='（暂不可用）'});$('otaState').textContent='暂不可用';$('otaMsg').textContent='固件暂不支持从网页触发检查和升级';$('aliKey').value='';$('otaUrl').value='';let save=document.createElement('button');save.className='d-btn';save.textContent='保存 OTA 地址';save.onclick=()=>run(async()=>{await api('/api/developer/ota',{url:$('otaUrl').value});toast('地址已保存')});$('otaUrl').after(save);
qa('.d-value').forEach(e=>e.textContent='--');qa('[data-panel="hub"] .d-diff-row').forEach(e=>e.remove());$('audioList').replaceChildren();$('fixedVoiceChips').replaceChildren();$('cloneList').replaceChildren();$('devVoiceName').textContent='--';qa('[data-panel="hub"] .d-card')[2]?.setAttribute('hidden','');qa('.d-api-row').forEach(b=>b.onclick=()=>navigator.clipboard?.writeText(b.dataset.copy).then(()=>toast('已复制')));gate();
})();
