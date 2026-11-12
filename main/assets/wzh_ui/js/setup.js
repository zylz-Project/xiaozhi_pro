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
      for (const ap of d.aps || []) {
        const row=document.createElement('button'); row.type='button'; row.className='net-row';
        const text=document.createElement('span'); text.className='net-copy';
        const title=document.createElement('b'); title.textContent=ap.ssid;
        const sub=document.createElement('small'); sub.textContent=ap.unavailable?'暂不可用':((ap.current?'当前连接 · ':ap.saved?'已保存 · ':'')+(ap.rssi == null?'信号未知':ap.rssi+' dBm'));
        text.append(title,sub); row.append(text); row.addEventListener('click',()=>{ selected=ap; $('pwTitle').textContent='连接到 '+ap.ssid; $('pwSsid').textContent=ap.ssid; $('pwInput').value=''; $('pwError').textContent=''; show('pwLayer',true); }); list.append(row);
      }
      $('scanMeta').textContent=(d.aps||[]).length+' 个网络';
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
  (async()=>{ try { const s=await api('/api/status'); online=!!s.wifi?.connected; const mode=online?'change':s.saved_network?'offline':'first'; $('setupPhone').dataset.mode=mode; $('setupBack').hidden=!online; $('currentNet').hidden=!online; $('currentSsid').textContent=s.wifi?.ssid||'--'; $('currentSub').textContent=online?'当前连接':'离线'; $('setupSub').textContent=online?'旧网络会保留；新网络连接失败后会恢复。':'选择家里的 2.4 GHz 网络。'; if(window.PandaProduct){const id=s.build==='wzh-dinosaur'?'dinosaur':s.build==='wzh-tailredpanda'?'redpanda':'crawlPanda';PandaProduct.set(id,false);} await scan(); if(window.PandaGuide) PandaGuide.auto(mode==='change'?'setupChange':mode==='offline'?'setupOffline':'setupFirst'); } catch(e){toast(e.message);} })();
})();
