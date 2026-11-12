/* 爬爬熊猫 · 主题同步（离线原型）
   一个设备一套外观：颜色主题 / 明暗 / 玻璃材质 / 控制台布局。
   入口只放在详情页；其他页面读取同一份 localStorage 并应用。
   接固件时把 read/save 换成 /api/appearance 即可。 */
(function (window, document) {
  'use strict';

  var KEY = 'panda-appearance-v1';
  var DEFAULTS = { theme: 'milk', material: 'b', layout: 'c', mode: 'light' };
  var LEGACY = {
    theme: 'panda-theme-panda',
    material: 'panda-mat-panda',
    layout: 'panda-layout-panda',
    mode: 'panda-mode'
  };
  var THEMES = ['milk', 'salt', 'berry', 'bamboo'];

  function normalize(raw) {
    var out = Object.assign({}, DEFAULTS);
    if (!raw || typeof raw !== 'object') return out;
    if (THEMES.indexOf(raw.theme) >= 0) out.theme = raw.theme;
    if (raw.material === 'a' || raw.material === 'b') out.material = raw.material;
    if (raw.layout === 'c' || raw.layout === 'd') out.layout = raw.layout;
    if (raw.mode === 'dark' || raw.mode === 'light') out.mode = raw.mode;
    return out;
  }

  function read() {
    try {
      var raw = localStorage.getItem(KEY);
      if (raw) return normalize(JSON.parse(raw));
      var old = {};
      var found = false;
      Object.keys(LEGACY).forEach(function (k) {
        var v = localStorage.getItem(LEGACY[k]);
        if (v) { old[k] = v; found = true; }
      });
      if (found) {
        var migrated = normalize(old);
        localStorage.setItem(KEY, JSON.stringify(migrated));
        return migrated;
      }
    } catch (e) {}
    return Object.assign({}, DEFAULTS);
  }

  function render(st) {
    document.querySelectorAll('.phone').forEach(function (phone) {
      phone.setAttribute('data-theme', st.theme);
      phone.classList.toggle('dark', st.mode === 'dark');
      phone.classList.toggle('a', st.material === 'a');
      phone.classList.toggle('b', st.material !== 'a');
      var con = phone.querySelector('[data-console]');
      if (con) con.setAttribute('data-layout', st.layout);
    });
    document.querySelectorAll('button[data-theme]').forEach(function (btn) {
      btn.setAttribute('aria-pressed', btn.getAttribute('data-theme') === st.theme ? 'true' : 'false');
    });
    document.querySelectorAll('button[data-mat]').forEach(function (btn) {
      btn.setAttribute('aria-pressed', btn.getAttribute('data-mat') === st.material ? 'true' : 'false');
    });
    document.querySelectorAll('.layout-btn').forEach(function (btn) {
      btn.setAttribute('aria-pressed', btn.getAttribute('data-layout') === st.layout ? 'true' : 'false');
    });
    document.querySelectorAll('button[data-mode]').forEach(function (btn) {
      btn.setAttribute('aria-pressed', btn.getAttribute('data-mode') === st.mode ? 'true' : 'false');
    });
  }

  function apply() {
    var st = read();
    render(st);
    try { document.dispatchEvent(new CustomEvent('panda:appearance', { detail: st })); } catch (e) {}
    return st;
  }

  function save(patch) {
    var st = normalize(Object.assign({}, read(), patch || {}));
    try { localStorage.setItem(KEY, JSON.stringify(st)); } catch (e) {}
    render(st);
    try { document.dispatchEvent(new CustomEvent('panda:appearance', { detail: st })); } catch (e) {}
    return st;
  }

  window.PandaTheme = {
    get: read,
    set: save,
    apply: apply,
    key: KEY
  };

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', apply);
  } else {
    apply();
  }
})(window, document);
