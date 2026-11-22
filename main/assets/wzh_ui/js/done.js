(function () {
  'use strict';
  const $ = id => document.getElementById(id);
  const primary = $('donePrimary');
  const later = $('doneLater');
  let busy = false;
  let entering = false;
  let phase = 'connecting';
  let timer;

  function read(key) {
    try { return sessionStorage.getItem(key) || ''; } catch (e) { return ''; }
  }

  function context(value) {
    try {
      if (value) sessionStorage.setItem('wzh_done', value);
      else {
        sessionStorage.removeItem('wzh_done');
        sessionStorage.removeItem('wzh_done_at');
      }
    } catch (e) { /* The completion cookie remains available. */ }
  }

  async function post(path) {
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), 4000);
    try {
      const response = await fetch(path, {
        method: 'POST', credentials: 'same-origin', signal: controller.signal,
        headers: {'Content-Type': 'application/json'}, body: JSON.stringify({ctx: read('wzh_done')})
      });
      const data = await response.json().catch(() => null);
      if (!response.ok || data?.ok !== true) {
        const error = new Error(data?.error?.message || '设备回复不完整，请重试');
        if (!response.ok) error.status = response.status;
        throw error;
      }
      return data;
    } catch (e) {
      if (e.name === 'AbortError' || e instanceof TypeError)
        throw new Error('暂时无法连接设备，请确认手机仍连着设备热点');
      throw e;
    } finally {
      clearTimeout(timer);
    }
  }

  function paint(next, message) {
    const changed = next !== phase;
    phase = next;
    const ready = phase === 'online';
    const failed = phase === 'failed' || phase === 'expired';
    const label = ready ? '已连接' : failed ? '无法继续' : '连接中';
    const ssid = read('wzh_setup_ssid');
    $('donePhone').classList.toggle('done-ok', ready);
    $('donePhone').classList.toggle('done-fail', failed);
    $('doneActions').setAttribute('aria-hidden', String(!ready && !failed));
    $('doneHeading').textContent = ready ? '网络连接成功' :
      phase === 'expired' ? '配网完成页已失效' : failed ? '网络连接失败' : '正在连接网络…';
    $('doneStatus').textContent = label;
    $('doneStatus2').textContent = ready && ssid ? '已连接 ' + ssid : label;
    $('doneChip').textContent = label;
    $('doneChip').classList.toggle('accent', ready);
    $('doneSsid').textContent = ssid || '所选网络';
    $('doneSub').textContent = message || (ready ?
      '保持手机连接设备热点，继续进入设备详情。' : '请保持手机连接设备热点。');
    $('doneNote').hidden = !ready;
    primary.disabled = entering || (!ready && !failed);
    primary.querySelector('span').textContent = ready ? '进入设备详情' :
      phase === 'expired' ? '前往登录' : '返回配网';
    later.textContent = failed ? '返回设备首页' : '稍后再看';
    $('doneGuide').hidden = !ready && !failed;
    if (changed && (ready || failed) && window.PandaGuide)
      PandaGuide.auto(ready ? 'doneSuccess' : 'doneFail', {manualOnly: true});
  }

  async function poll() {
    clearTimeout(timer);
    if (busy || entering || document.hidden || phase !== 'connecting') return;
    busy = true;
    try {
      const data = await post('/api/setup/status');
      if (data.ctx) context(data.ctx);
      const next = data.phase === 'failed' ? 'failed' :
        data.phase === 'online' && data.net_state === 'ONLINE' ? 'online' : 'connecting';
      paint(next, data.error?.message);
    } catch (e) {
      if (e.status === 401) {
        context('');
        paint('expired', '可以登录设备详情查看当前状态。');
      } else {
        $('doneSub').textContent = e.message;
      }
    } finally {
      busy = false;
      if(!document.hidden && phase==='connecting') timer=setTimeout(poll,1000);
    }
  }

  primary.onclick = async () => {
    if (entering) return;
    if (phase === 'failed' || phase === 'expired') {
      location.href = phase === 'expired' ? '/login?next=/' : '/setup';
      return;
    }
    if (phase !== 'online') return;
    entering = true;
    primary.disabled = later.disabled = true;
    try {
      for (let attempt = 0; ; ++attempt) {
        try { await post('/api/enter'); break; }
        catch (error) {
          if (attempt === 2 || (error.status && error.status !== 409 && error.status < 500))
            throw error;
          $('doneSub').textContent = '正在恢复与设备的连接…';
          await new Promise(resolve => setTimeout(resolve, 700));
        }
      }
      context('');
      location.replace('/');
    } catch (e) {
      entering = false;
      later.disabled = false;
      if (e.status === 401) {
        context('');
        paint('expired', '可以登录设备详情查看当前状态。');
      } else {
        primary.disabled = false;
        $('doneSub').textContent = e.message;
      }
    }
  };
  later.onclick = () => { location.href = '/'; };
  const handoffAge = Date.now() - Number(read('wzh_done_at'));
  paint(read('wzh_done') && handoffAge >= 0 && handoffAge < 30000 ? 'online' : 'connecting');
  window.addEventListener('pageshow', poll);
  document.addEventListener('visibilitychange', poll);
  poll();
})();
