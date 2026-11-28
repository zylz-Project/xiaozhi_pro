(function () {
  'use strict';
  const $ = id => document.getElementById(id);
  let online = false, authenticated = false, selected = null, busy = false, scanning = false;
  let initialized = false, checkingStatus = false, switchNeedsRetry = false, wifiTransport = null;
  const wait = ms => new Promise(resolve => setTimeout(resolve, ms));
  async function api(path, body, timeoutMs=10000) {
    const opts = {cache:'no-store', credentials:'same-origin'};
    if (body !== undefined) { opts.method='POST'; opts.headers={'Content-Type':'application/json'}; opts.body=JSON.stringify(body); }
    const controller = new AbortController(), timer = setTimeout(()=>controller.abort(),timeoutMs);
    let r, d;
    try { r = await fetch(path, {...opts, signal:controller.signal}); d = await r.json().catch(e=>{ if(e.name==='AbortError')throw e; return null; }); }
    catch(e) { if(e.name==='AbortError') throw new Error('连接设备超时，请确认手机仍连接设备热点后重试'); throw e; }
    finally { clearTimeout(timer); }
    if (!r.ok || d?.ok === false) { const e = new Error(d?.error?.message || '操作失败'); e.status = r.status; throw e; }
    if (!d || typeof d!=='object' || Array.isArray(d)) throw new Error('设备回复未接收完整，请稍后重试');
    return d;
  }
  function show(id, visible) {
    const el=$(id);
    if (!visible) WzhEditorFocus.blurWithin(el);
    el.toggleAttribute('inert', !visible);
    el.classList.toggle('show', visible);
    el.setAttribute('aria-hidden', String(!visible));
  }
  function save(key, value) {
    try { sessionStorage.setItem(key, value); } catch (e) { /* The device also sends a cookie. */ }
  }
  let toastTimer;
  function toast(msg) {
    clearTimeout(toastTimer);
    $('toast').textContent=msg; $('toast').classList.add('show');
    toastTimer=setTimeout(()=>$('toast').classList.remove('show'),2500);
  }
  function loginFor(e) {
    if (!e || e.status !== 401) return false;
    location.href='/login?next=/setup';
    return true;
  }
  function ensureWifiTransport(net) {
    if(!wifiTransport)wifiTransport=(async()=>{
      return WzhNetworkTransport.select('wifi',{
        initial:net||await api('/api/status',undefined,3000),
        request:transport=>api('/api/network/transport',{transport},5000),
        read:timeout=>api('/api/status',undefined,timeout),
        progress:message=>{ $('scanMeta').textContent=message; }
      });
    })().finally(()=>{ wifiTransport=null; });
    return wifiTransport;
  }
  async function scan(network) {
    if (scanning||busy) return;
    scanning=true;
    $('scanBtn').disabled=true; $('scanBtn').classList.add('spin');
    $('scanMeta').textContent='正在扫描附近网络…';
    $('netEmpty').textContent='还没有发现附近的网络，请点击右上角刷新';
    $('netEmpty').hidden=true;
    const list=$('netList'), hadResults=list.childElementCount>0;
    list.setAttribute('aria-busy','true');
    try {
      if(!network)await ensureWifiTransport();
      // The firmware fills its scan cache asynchronously, so a single request
      // often returns the previous (empty) result. A scan can also briefly
      // switch the AP channel, so tolerate short fetch failures while the
      // phone rejoins and keep polling instead of abandoning the list.
      let aps=[], answered=false, lastError=null;
      const scanDeadline=Date.now()+15000;
      while (!busy&&Date.now()<scanDeadline) {
        try {
          const d=await api('/scan',undefined,Math.min(3000,scanDeadline-Date.now()));
          answered=true; lastError=null;
          aps=(d.aps||[]).filter(ap=>!(online&&ap.current));
          if (aps.length) break;
        } catch(e) {
          if(e.status) throw e;
          lastError=e;
        }
        const remaining=scanDeadline-Date.now();
        if (remaining>0) await wait(Math.min(700,remaining));
      }
      if(busy)return;
      if(!aps.length&&hadResults){
        $('scanMeta').textContent='暂未收到新结果，仍显示上次扫描';
        return;
      }
      // The current network already has its own card at the top of the page.
      const strength=ap=>ap.unavailable?'暂不可用':ap.rssi==null?'信号未知':ap.rssi>=-55?'信号很强':ap.rssi>=-67?'信号强':ap.rssi>=-75?'信号一般':'信号较弱';
      const bars=ap=>ap.rssi==null?0:ap.rssi>=-55?4:ap.rssi>=-67?3:ap.rssi>=-75?2:1;
      const rows=document.createDocumentFragment();
      for (const ap of aps) {
        const row=document.createElement('button'); row.type='button'; row.className='net-row';
        if (ap.band===5) row.classList.add('is-5g');
        const sig=document.createElement('span'); sig.className='net-sig'; sig.setAttribute('aria-hidden','true');
        const on=bars(ap);
        for (let i=0;i<4;i++){ const bar=document.createElement('i'); if(i<on) bar.className='on'; sig.append(bar); }
        const text=document.createElement('span'); text.className='net-copy';
        const title=document.createElement('b'); title.textContent=ap.ssid;
        if (ap.saved) { const tag=document.createElement('span'); tag.className='net-tag'; tag.textContent='已保存'; title.append(tag); }
        if (ap.auth != null && ap.auth !== 0) {
          const lock=document.createElementNS('http://www.w3.org/2000/svg','svg'); lock.setAttribute('class','ico'); lock.setAttribute('viewBox','0 0 24 24');
          const use=document.createElementNS('http://www.w3.org/2000/svg','use'); use.setAttribute('href','#i-lock'); lock.append(use); title.append(lock);
        }
        const sub=document.createElement('small'); sub.textContent=strength(ap);
        text.append(title,sub);
        const band=document.createElement('span'); band.className='net-band'; band.textContent=ap.band===5?'5G':ap.unavailable?'频段未知':'2.4G';
        const chev=document.createElementNS('http://www.w3.org/2000/svg','svg'); chev.setAttribute('class','chev'); chev.setAttribute('viewBox','0 0 24 24'); chev.setAttribute('fill','none'); chev.setAttribute('stroke','currentColor'); chev.setAttribute('stroke-width','1.9');
        const cu=document.createElementNS('http://www.w3.org/2000/svg','use'); cu.setAttribute('href','#i-chev'); chev.append(cu);
        row.append(sig,text,band,chev);
        row.addEventListener('click',()=>{
          selected=ap;
          const open=ap.auth===0;
          $('pwLayer').dataset.openNetwork=String(open);
          $('pwTitle').textContent='连接到 '+ap.ssid;
          $('pwSsid').textContent=ap.ssid;
          $('pwSub').textContent=open?'这是开放网络，可直接连接':'输入这个 Wi-Fi 的密码';
          $('pwInput').value='';
          $('pwInput').type='password';
          $('pwInput').placeholder=open?'无需密码':'请输入 Wi-Fi 密码';
          $('pwPeek').setAttribute('aria-label','显示密码');
          $('pwError').textContent='';
          show('pwLayer',true);
        });
        rows.append(row);
      }
      list.replaceChildren(rows);
      $('scanMeta').textContent=aps.length+' 个网络';
      $('netEmpty').hidden=aps.length>0;
      if (!aps.length && !answered && lastError)
        $('netEmpty').textContent='扫描暂时中断，请确认手机仍连接设备热点后重试';
    } catch(e) {
      if(!loginFor(e)) {
        $('scanMeta').textContent=e.message;
        $('netEmpty').textContent='扫描失败，请确认手机仍连接设备热点后重试';
        $('netEmpty').hidden=hadResults;
      }
    }
    finally {
      scanning=false;
      list.setAttribute('aria-busy','false');
      $('scanBtn').disabled=busy; $('scanBtn').classList.remove('spin');
    }
  }
  async function connect(ssid,password,error,button) {
    if (busy) { error.textContent='上一项连接还在进行，请稍候'; return; } if (!ssid || new TextEncoder().encode(ssid).length>32 || new TextEncoder().encode(password).length>64) { error.textContent='请检查网络名称和密码'; return; }
    WzhEditorFocus.blurWithin(button.closest('.layer'));
    busy=true; button.disabled=true; $('scanBtn').disabled=true;
    error.textContent='正在连接，请保持手机连接设备热点…';
    try {
      await ensureWifiTransport();
      let job=window.crypto?.getRandomValues ? Array.from(window.crypto.getRandomValues(new Uint8Array(16)),
        byte=>byte.toString(16).padStart(2,'0')).join('') : undefined;
      let submitError;
      try {
        const r=await api('/submit',{ssid,password,job});
        job=r.job||job;
      } catch(e) {
        if(!job||e.status)throw e;
        submitError=e;
        error.textContent='连接暂时中断，正在查询配网进度…';
      }
      if(!job)throw new Error('设备未返回配网任务，请重试');
      save('wzh_setup_ssid',ssid);
      const deadline=Date.now()+120000;
      const jobStatus='/api/setup/status?job='+encodeURIComponent(job);
      while(Date.now()<deadline) {
        let s;
        try { s=await api(jobStatus,undefined,3000); }
        catch(e) {
          if(e.status===401)throw submitError||e;
          error.textContent='连接暂时中断，正在重新查询…';
        }
        if(s?.phase==='failed')throw new Error(s.error?.message||'连接失败');
        if(s?.phase==='online'&&s.net_state==='ONLINE'&&s.ctx){
          save('wzh_done',s.ctx);
          save('wzh_done_at',String(Date.now()));
          location.replace('/done');
          return;
        }
        await wait(750);
      }
      throw new Error('连接状态查询超时，请确认手机连接设备热点后重试');
    } catch(e) { if(!loginFor(e)) error.textContent=e.message; }
    finally { busy=false; button.disabled=false; $('scanBtn').disabled=scanning; }
  }
  $('pwConnect').onclick=()=>connect(selected?.ssid || '', $('pwInput').value, $('pwError'), $('pwConnect'));
  $('manualConnect').onclick=()=>connect($('manualSsid').value,$('manualPw').value,$('manualError'),$('manualConnect'));
  for (const [field, button] of [['pwInput','pwConnect'],['manualSsid','manualConnect'],['manualPw','manualConnect']]) {
    $(field).addEventListener('keydown', e=>{
      if (e.key==='Enter' && !e.isComposing) { e.preventDefault(); $(button).click(); }
    });
  }
  $('scanBtn').onclick=()=>{
    switchNeedsRetry=false;
    return initialized ? scan() : refreshStatus();
  };
  $('manualSsidBtn').onclick=()=>show('manualLayer',true); $('setupBack').onclick=()=>location.href=authenticated?'/':'/login?next=/';
  document.querySelectorAll('[data-close]').forEach(b=>b.onclick=()=>{if(busy){toast('正在连接，请稍候');return;}show(b.dataset.close,false);});
  ['pw','manual'].forEach(prefix=>$(prefix+'Peek').onclick=()=>{
    const input=$(prefix==='pw'?'pwInput':'manualPw');
    input.type=input.type==='password'?'text':'password';
    $(prefix+'Peek').setAttribute('aria-label',input.type==='password'?'显示密码':'隐藏密码');
  });
  function showStartupNotice(s) { const el=$('startupNotice'); const text=s.startup_notice||''; el.textContent=text; el.hidden=!text; }
  async function refreshStatus() {
    if (document.hidden || checkingStatus || busy) return;
    checkingStatus = true;
    let switching = false;
    try {
      const status = await api('/api/status', undefined, 3000);
      showStartupNotice(status);
      if (initialized) return;
      const dinosaur = status.build === 'wzh-dinosaur';
      authenticated = status.authenticated??!!status.wifi?.connected;
      $('setupBack').hidden = !(authenticated||status.wifi?.connected||dinosaur);
      if(switchNeedsRetry&&(status.transport!=='wifi'||status.transport_switch?.state==='pending'))return;
      switching=dinosaur&&(status.transport!=='wifi'||status.transport_switch?.state==='pending');
      const network = await ensureWifiTransport(status);
      switchNeedsRetry=false;
      online = !!network.wifi?.connected;
      authenticated = network.authenticated??online;
      const mode = online ? 'change' : network.saved_network ? 'offline' : 'first';
      $('setupPhone').dataset.mode = mode;
      $('setupBack').hidden = !(authenticated||online||dinosaur);
      $('currentNet').hidden = !online;
      $('currentSsid').textContent = network.wifi?.ssid || '--';
      $('currentSub').textContent = online ? '当前连接' : '离线';
      const copy = {
        change: {kicker:'CHANGE NETWORK · 更换网络', title:'换一个 Wi-Fi',
          sub:'选一个新的 2.4GHz 网络。连不上会自动回到现在的网络。', steps:['选网络','输密码','等待连接']},
        offline: {kicker:'RECONNECT · 重新联网', title:'重新连上 Wi-Fi',
          sub:'设备现在不在线。选一个网络重新连上，原来的凭据会保留。', steps:['选网络','输密码','等待连接']},
        first: {kicker:'FIRST CONNECTION · 第一次配网', title:'连上家里的 Wi-Fi',
          sub:'只支持 2.4GHz。选一个网络，输入密码就好。', steps:['连热点','选网络','输密码']}
      }[mode];
      $('setupKicker').textContent = copy.kicker;
      $('setupHeading').textContent = copy.title;
      $('setupSub').textContent = copy.sub;
      document.querySelectorAll('.setup-flow span').forEach((el, i) => {
        el.innerHTML = '<b>' + (i + 1) + '</b>' + (copy.steps[i] || '');
        el.classList.toggle('on', mode === 'first' ? i < 2 : i < 1);
      });
      const stepCount = document.querySelector('.step-count');
      if (stepCount) stepCount.textContent = (mode === 'first' ? '01' : '02') + ' / 03';
      if (window.PandaProduct) {
        PandaProduct.updateDeviceStatus(network);
      }
      initialized = true;
      await scan(network);
      if (window.PandaGuide)
        PandaGuide.auto(mode === 'change' ? 'setupChange' : mode === 'offline' ? 'setupOffline' : 'setupFirst');
    } catch (e) {
      if (!initialized && !loginFor(e)) {
        if(switching)switchNeedsRetry=true;
        $('scanMeta').textContent=switching?e.message:'正在恢复与设备的连接…';
        $('netEmpty').textContent=switching?'请保持手机连接设备热点，点击刷新可重试'
                                         :'请确认手机仍连接设备热点，也可以点击刷新';
        $('netEmpty').hidden=false;
      }
    } finally {
      checkingStatus = false;
    }
  }
  window.addEventListener('pageshow', refreshStatus);
  document.addEventListener('visibilitychange', refreshStatus);
  setInterval(refreshStatus,5000);
  refreshStatus();
})();
