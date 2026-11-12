(function () {
  'use strict';
  const $ = id => document.getElementById(id);
  let busy = false;
  $('loginCard').addEventListener('submit', async e => {
    e.preventDefault();
    if (busy) return;
    const input = $('loginPw'), button = $('loginSubmit'), error = $('loginError');
    if (!input.value) { error.textContent = '请输入页面密码'; return; }
    busy = true;
    button.disabled = true;
    error.textContent = '正在验证，请稍候…';
    input.blur();
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), 15000);
    try {
      const r = await fetch('/api/login', {
        method: 'POST', credentials: 'same-origin', signal: controller.signal,
        headers: {'Content-Type': 'application/json'}, body: JSON.stringify({password: input.value})
      });
      const d = await r.json().catch(() => ({}));
      if (!r.ok || d.ok === false) throw new Error(d.error?.message || '登录失败，请重试');
      input.value = '';
      const next = new URLSearchParams(location.search).get('next');
      location.replace(next === '/setup' || next === '/manual' ? next : '/');
    } catch (err) {
      error.textContent = err.name === 'AbortError' ? '连接超时，请确认手机连接设备热点后重试' :
        (err instanceof TypeError ? '连接失败，请确认手机连接设备热点后重试' : err.message);
    } finally {
      clearTimeout(timer);
      busy = false;
      button.disabled = false;
    }
  });
  $('loginPeek').onclick = () => { const input = $('loginPw'); input.type = input.type === 'password' ? 'text' : 'password'; };
  if (window.PandaGuide) PandaGuide.auto('login', {manualOnly: true});
})();
