(function () {
  'use strict';
  const $ = id => document.getElementById(id);
  const params = new URLSearchParams(location.search);
  let online = false, selected = null, busy = false;
  const wait = ms => new Promise(resolve => setTimeout(resolve, ms));
  async function api(path, body) {
    const opts = {cache:'no-store', credentials:'same-origin'};
    if (body !== undefined) { opts.method='POST'; opts.headers={'Content-Type':'application/json'}; opts.body=JSON.stringify(body); }
    const r = await fetch(path, opts), d = await r.json();
    if (!r.ok || d.ok === false) { const e = new Error(d.error?.message || '操作失败'); e.status = r.status; throw e; }
    return d;
  }
  function show(id, visible) { const el=$(id); el.classList.toggle('show',visible); el.setAttribute('aria-hidden', visible ? 'false' : 'true'); }
  function toast(msg) { $('toast').textContent=msg; $('toast').classList.add('show'); setTimeout(()=>$('toast').classList.remove('show'),2500); }
  async function scan() {
    $('scanBtn').disabled=true; $('scanMeta').textContent='正在扫描…';
    try {
      let d=await api('/scan');
      if (!d.aps?.length) { await wait(1200); d=await api('/scan'); }
      const list=$('netList'); list.textContent='';
      // The current network already has its own card at the top of the page.
      const aps=(d.aps||[]).filter(ap=>!(online&&ap.current));
      const strength=ap=>ap.unavailable?'暂不可用':ap.rssi==null?'信号未知':ap.rssi>=-55?'信号很强':ap.rssi>=-67?'信号强':ap.rssi>=-75?'信号一般':'信号较弱';
      const bars=ap=>ap.rssi==null?0:ap.rssi>=-55?4:ap.rssi>=-67?3:ap.rssi>=-75?2:1;
      for (const ap of aps) {
        const row=document.createElement('button'); row.type='button'; row.className='net-row';
        if (ap.band===5 || ap.unavailable) row.classList.add('is-5g');
        const sig=document.createElement('span'); sig.className='net-sig'; sig.setAttribute('aria-hidden','true');
        const on=bars(ap);
        for (let i=0;i<4;i++){ const bar=document.createElement('i'); if(i<on) bar.className='on'; sig.append(bar); }
        const text=document.createElement('span'); text.className='net-copy';
        const title=document.createElement('b'); title.textContent=ap.ssid;
        if (ap.saved) { const tag=document.createElement('span'); tag.className='net-tag'; tag.textContent='已保存'; title.append(tag); }
        const lock=document.createElementNS('http://www.w3.org/2000/svg','svg'); lock.setAttribute('class','ico'); lock.setAttribute('viewBox','0 0 24 24');
        const use=document.createElementNS('http://www.w3.org/2000/svg','use'); use.setAttribute('href','#i-lock'); lock.append(use); title.append(lock);
        const sub=document.createElement('small'); sub.textContent=strength(ap);
        text.append(title,sub);
        const band=document.createElement('span'); band.className='net-band'; band.textContent=(ap.band===5||ap.unavailable)?'5G':'2.4G';
        const chev=document.createElementNS('http://www.w3.org/2000/svg','svg'); chev.setAttribute('class','chev'); chev.setAttribute('viewBox','0 0 24 24'); chev.setAttribute('fill','none'); chev.setAttribute('stroke','currentColor'); chev.setAttribute('stroke-width','1.9');
        const cu=document.createElementNS('http://www.w3.org/2000/svg','use'); cu.setAttribute('href','#i-chev'); chev.append(cu);
        row.append(sig,text,band,chev);
        row.addEventListener('click',()=>{ selected=ap; $('pwTitle').textContent='连接到 '+ap.ssid; $('pwSsid').textContent=ap.ssid; $('pwInput').value=''; $('pwError').textContent=''; show('pwLayer',true); });
        list.append(row);
      }
      $('scanMeta').textContent=aps.length+' 个网络';
    } catch(e) { $('scanMeta').textContent=e.message; }
    finally { await wait(4000); $('scanBtn').disabled=false; }
  }
  async function connect(ssid,password,error,button) {
    if (busy) return; if (!ssid || new TextEncoder().encode(ssid).length>32 || new TextEncoder().encode(password).length>64) { error.textContent='请检查网络名称和密码'; return; }
    busy=true; button.disabled=true; error.textContent='正在连接，请保持手机连接设备热点…';
    try {
      const r=await api('/submit',{ssid,password}); sessionStorage.setItem('wzh_setup_ssid',ssid);
      for (;;) { await wait(1000); const s=await api('/api/setup/status?job='+encodeURIComponent(r.job)); if (s.phase==='failed') throw new Error(s.error?.message||'连接失败'); if(s.ctx){sessionStorage.setItem('wzh_done',s.ctx);location.href='/done';return;} }
    } catch(e) { error.textContent=e.message; if(e.status===401 && online) location.href='/login?next=/setup'; }
    finally { busy=false; button.disabled=false; }
  }
  $('pwConnect').onclick=()=>connect(selected?.ssid || '', $('pwInput').value, $('pwError'), $('pwConnect'));
  $('manualConnect').onclick=()=>connect($('manualSsid').value,$('manualPw').value,$('manualError'),$('manualConnect'));
  $('scanBtn').onclick=scan; $('manualSsidBtn').onclick=()=>show('manualLayer',true); $('setupBack').onclick=()=>location.href='/';
  document.querySelectorAll('[data-close]').forEach(b=>b.onclick=()=>show(b.dataset.close,false));
  ['pw','manual'].forEach(prefix=>$(prefix+'Peek').onclick=()=>{ const input=$(prefix==='pw'?'pwInput':'manualPw'); input.type=input.type==='password'?'text':'password'; });
  function showStartupNotice(s) { const el=$('startupNotice'); const text=s.startup_notice||''; el.textContent=text; el.hidden=!text; }
  async function refreshStartupNotice(){if(document.hidden)return;try{showStartupNotice(await api('/api/status'));}catch(e){ /* Connection status is handled by the existing setup flow. */ }}
  setInterval(refreshStartupNotice,5000);
  (async()=>{ try { const s=await api('/api/status'); showStartupNotice(s); online=!!s.wifi?.connected; const mode=online?'change':s.saved_network?'offline':'first'; $('setupPhone').dataset.mode=mode; $('setupBack').hidden=!online; $('currentNet').hidden=!online; $('currentSsid').textContent=s.wifi?.ssid||'--'; $('currentSub').textContent=online?'当前连接':'离线';
        const copy={change:{kicker:'CHANGE NETWORK · 更换网络',title:'换一个 Wi-Fi',sub:'选一个新的 2.4GHz 网络。连不上会自动回到现在的网络。',steps:['选网络','输密码','等待连接']},offline:{kicker:'RECONNECT · 重新联网',title:'重新连上 Wi-Fi',sub:'设备现在不在线。选一个网络重新连上，原来的凭据会保留。',steps:['选网络','输密码','等待连接']},first:{kicker:'FIRST CONNECTION · 第一次配网',title:'连上家里的 Wi-Fi',sub:'只支持 2.4GHz。选一个网络，输入密码就好。',steps:['连热点','选网络','输密码']}}[mode];
        $('setupKicker').textContent=copy.kicker; $('setupHeading').textContent=copy.title; $('setupSub').textContent=copy.sub;
        document.querySelectorAll('.setup-flow span').forEach((el,i)=>{ el.innerHTML='<b>'+(i+1)+'</b>'+(copy.steps[i]||''); el.classList.toggle('on', mode==='first' ? i<2 : i<1); });
        const stepCount=document.querySelector('.step-count'); if(stepCount) stepCount.textContent=(mode==='first'?'01':'02')+' / 03'; if(window.PandaProduct){const id=s.build==='wzh-dinosaur'?'dinosaur':s.build==='wzh-tailredpanda'?'redpanda':'crawlPanda';PandaProduct.set(id,false);} await scan(); if(window.PandaGuide) PandaGuide.auto(mode==='change'?'setupChange':mode==='offline'?'setupOffline':'setupFirst'); } catch(e){toast(e.message);} })();
})();
