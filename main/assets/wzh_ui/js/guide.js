/* 爬爬熊猫 · 首次使用引导引擎（离线原型）
   真实固件接入时，把 window.PANDA_GUIDE_API_BASE 指向设备地址即可。
   接口契约见 docs/ui/wzh_onboarding_redesign_2026-11-10.md：
     GET  /api/guide/state?device=<id>  -> { device, seen: { setup, done, login, detail, manual } }
     POST /api/guide/state              -> { device, page, seen: true }
     POST /api/guide/reset              -> { device } 清空该设备记录
   未配置 API_BASE 时使用 localStorage 的 panda-guide-v1 兜底。 */
(function (window, document) {
  'use strict';

  var STORE_KEY = 'panda-guide-v1:' + (window.PandaProduct ? PandaProduct.id : 'crawlPanda');
  var DEVICE_KEY = 'panda-device-id';
  var STYLE_KEY = 'panda-guide-style';
  var STYLES = ['warm', 'sticker', 'cloud', 'glass', 'ticket', 'leaf', 'lamp', 'comic', 'sticky', 'minimal'];
  var API_BASE = window.PANDA_GUIDE_API_BASE || '';
  var CONFIG = window.PANDA_GUIDE_CONFIG || {};
  var params = new URLSearchParams(window.location.search);

  var state = readLocal();
  var phoneEl = null;
  var rootEl = null;
  var spotEl = null;
  var bubbleEl = null;
  var hintEl = null;
  var key = null;
  var config = null;
  var steps = [];
  var index = 0;
  var lastFocus = null;
  var scrollRaf = 0;
  var resizeTimer = 0;
  var followTimer = 0;
  var actionCleanup = null;
  var gateCleanup = null;
  var advancing = false;
  var locked = false;
  var guardBound = false;

  function deviceId() {
    var fromUrl = params.get('device');
    if (fromUrl) return fromUrl;
    try {
      var saved = localStorage.getItem(DEVICE_KEY);
      if (saved) return saved;
      var made = 'demo-' + Math.random().toString(36).slice(2, 8);
      localStorage.setItem(DEVICE_KEY, made);
      return made;
    } catch (e) {
      return 'demo-device';
    }
  }

  function readLocal() {
    var id = deviceId();
    try {
      var raw = localStorage.getItem(STORE_KEY);
      if (raw) {
        var parsed = JSON.parse(raw);
        if (parsed && parsed.seen) {
          return { device: parsed.device || id, seen: parsed.seen, updatedAt: parsed.updatedAt || 0 };
        }
      }
    } catch (e) {}
    return { device: id, seen: {}, updatedAt: 0 };
  }

  function writeLocal(next) {
    state = next;
    try { localStorage.setItem(STORE_KEY, JSON.stringify(next)); } catch (e) {}
  }

  function remoteGet() {
    if (!API_BASE) return;
    fetch(API_BASE + '/api/guide/state?device=' + encodeURIComponent(state.device))
      .then(function (res) { return res.ok ? res.json() : null; })
      .then(function (data) {
        if (data && data.seen) {
          writeLocal({ device: data.device || state.device, seen: data.seen, updatedAt: Date.now() });
        }
      })
      .catch(function () {});
  }

  function remoteMark(page) {
    if (!API_BASE) return;
    fetch(API_BASE + '/api/guide/state', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ device: state.device, page: page, seen: true })
    }).catch(function () {});
  }

  function seen(page) {
    return !!(state.seen && state.seen[page]);
  }

  function markSeen(page) {
    if (!page || seen(page)) return;
    var next = { device: state.device, seen: Object.assign({}, state.seen), updatedAt: Date.now() };
    next.seen[page] = true;
    writeLocal(next);
    remoteMark(page);
  }

  function activeStyle() {
    var asked = params.get('guideStyle') || params.get('guide-style') || params.get('style');
    if (asked && STYLES.indexOf(asked) >= 0) return asked;
    try {
      var saved = localStorage.getItem(STYLE_KEY);
      if (saved && STYLES.indexOf(saved) >= 0) return saved;
    } catch (e) {}
    return 'warm';
  }

  function setStyle(name) {
    if (STYLES.indexOf(name) < 0) return false;
    try { localStorage.setItem(STYLE_KEY, name); } catch (e) {}
    if (bubbleEl) bubbleEl.setAttribute('data-style', name);
    return true;
  }

  function ico(name) {
    if (name === 'arrow') return '<svg class="ico" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="m9 18 6-6-6-6"/></svg>';
    if (name === 'paw') return '<svg class="ico" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"><circle cx="6.4" cy="9.3" r="2.1"/><circle cx="17.6" cy="9.3" r="2.1"/><circle cx="9.5" cy="5.4" r="1.8"/><circle cx="14.5" cy="5.4" r="1.8"/><path d="M7.2 15.7c0-2.7 2.2-4.7 4.8-4.7s4.8 2 4.8 4.7c0 2.2-1.7 3.7-4.8 3.7s-4.8-1.5-4.8-3.7Z"/></svg>';
    return '';
  }

  function inSpot(x, y) {
    if (!spotEl) return false;
    var r = spotEl.getBoundingClientRect();
    if (x < r.left || x > r.right || y < r.top || y > r.bottom) return false;
    var radius = 18;
    if (spotEl.getAttribute('data-round') === 'true') radius = Math.min(r.width, r.height) / 2;
    if (!radius) return true;
    var dx = 0;
    var dy = 0;
    if (x < r.left + radius) dx = (r.left + radius) - x;
    else if (x > r.right - radius) dx = x - (r.right - radius);
    if (y < r.top + radius) dy = (r.top + radius) - y;
    else if (y > r.bottom - radius) dy = y - (r.bottom - radius);
    return !(dx > 0 && dy > 0) || dx * dx + dy * dy <= radius * radius;
  }

  function installGuard() {
    if (guardBound) return;
    guardBound = true;
    document.addEventListener('click', function (event) {
      if (!rootEl) return;
      if (event.target.closest && event.target.closest('.pg-bubble')) return;
      if (inSpot(event.clientX, event.clientY)) return;
      event.preventDefault();
      event.stopImmediatePropagation();
    }, true);
  }

  function buildRoot() {
    var wrap = document.createElement('div');
    wrap.className = 'pg-root';
    wrap.setAttribute('role', 'dialog');
    wrap.setAttribute('aria-modal', 'false');
    var product = window.PandaProduct ? PandaProduct.get() : null;
    wrap.setAttribute('aria-label', (product ? product.name : '爬爬熊猫') + '使用引导');
    wrap.innerHTML =
      '<div class="pg-block" data-side="top" aria-hidden="true"></div>' +
      '<div class="pg-block" data-side="bottom" aria-hidden="true"></div>' +
      '<div class="pg-block" data-side="left" aria-hidden="true"></div>' +
      '<div class="pg-block" data-side="right" aria-hidden="true"></div>' +
      '<div class="pg-spot" aria-hidden="true"></div>' +
      '<div class="pg-bubble" data-place="below" data-style="' + activeStyle() + '">' +
        '<div class="pg-head">' +
          '<span class="pg-avatar"><img src="' + (product ? product.assets.face : 'assets/crawlPanda_icon.webp') + '" alt=""></span>' +
          '<div class="pg-meta"><p class="pg-kicker">' + (product ? product.code : 'PANDA') + ' GUIDE</p><div class="pg-who">' + (product ? product.name : '爬爬熊猫') + ' · 使用引导</div></div>' +
          '<span class="pg-badge"></span>' +
        '</div>' +
        '<h3 class="pg-title" id="pgTitle"></h3>' +
        '<p class="pg-text"></p>' +
        '<p class="pg-hint" hidden></p>' +
        '<div class="pg-foot">' +
          '<button class="pg-btn pg-skip" type="button">先不看</button>' +
          '<div class="pg-dots" aria-hidden="true"></div>' +
          '<button class="pg-btn pg-next" type="button">下一步 ' + ico('arrow') + '</button>' +
        '</div>' +
        '<p class="pg-tip">' + ico('paw') + '只有第一次出现 · 右上角的小图标可以重看</p>' +
      '</div>';
    return wrap;
  }

  function scrollParent(target) {
    return target.closest('.console-scroll, .sheet-more, .scroll-area, .sheet, .console-panels, .setup-sheet, .manual-scroll, .done-body, .login-body');
  }

  function ensureVisible(target) {
    var sc = scrollParent(target);
    if (!sc || sc.scrollHeight <= sc.clientHeight + 2) return;
    var tr = target.getBoundingClientRect();
    var sr = sc.getBoundingClientRect();
    if (tr.top >= sr.top + 12 && tr.bottom <= sr.bottom - 12) return;
    var top = sc.scrollTop + (tr.top - sr.top) - Math.max(12, (sc.clientHeight - tr.height) / 2);
    var max = sc.scrollHeight - sc.clientHeight;
    top = Math.max(0, Math.min(max, top));
    try {
      sc.scrollTo({ top: top, behavior: 'smooth' });
    } catch (e) {
      sc.scrollTop = top;
    }
    // The document's capturing scroll listener already observes this scroller.
    setTimeout(scheduleLayout, 1000);
    followScroll(900);
  }

  function followScroll(ms) {
    if (followTimer) cancelAnimationFrame(followTimer);
    var end = Date.now() + ms;
    function tick() {
      scheduleLayout();
      if (Date.now() < end) followTimer = requestAnimationFrame(tick);
      else followTimer = 0;
    }
    followTimer = requestAnimationFrame(tick);
  }

  function layout() {
    if (!rootEl || !spotEl || !bubbleEl || !phoneEl) return;
    var step = steps[index];
    if (!step) return;
    var target = phoneEl.querySelector(step.target);
    if (!target) return;
    var pr = phoneEl.getBoundingClientRect();
    var tr = target.getBoundingClientRect();
    if (!tr.width && !tr.height) return;
    // Read all geometry before touching styles to avoid forcing a second layout.
    var bw = bubbleEl.offsetWidth;
    var bh = bubbleEl.offsetHeight;
    var pad = 5;
    var left = tr.left - pr.left - pad;
    var top = tr.top - pr.top - pad;
    var w = tr.width + pad * 2;
    var h = tr.height + pad * 2;
    spotEl.style.left = left + 'px';
    spotEl.style.top = top + 'px';
    spotEl.style.width = w + 'px';
    spotEl.style.height = h + 'px';
    var round = step.round || /^(BUTTON|A|SUMMARY)$/.test(target.tagName) || target.classList.contains('net-row');
    spotEl.setAttribute('data-round', round ? 'true' : 'false');

    // Four dimming blocks leave exactly the spotlight hole; everything else is click-proof.
    var bx = Math.max(0, left);
    var by = Math.max(0, top);
    var br = Math.min(pr.width, left + w);
    var bb = Math.min(pr.height, top + h);
    var holeW = Math.max(0, br - bx);
    var holeH = Math.max(0, bb - by);
    var blocks = {
      top: [0, 0, pr.width, by],
      bottom: [0, bb, pr.width, Math.max(0, pr.height - bb)],
      left: [0, by, bx, holeH],
      right: [br, by, Math.max(0, pr.width - br), holeH]
    };
    rootEl.querySelectorAll('.pg-block').forEach(function (block) {
      var r = blocks[block.getAttribute('data-side')];
      if (!r) return;
      block.style.left = r[0] + 'px';
      block.style.top = r[1] + 'px';
      block.style.width = r[2] + 'px';
      block.style.height = r[3] + 'px';
    });

    var phoneW = pr.width;
    var phoneH = pr.height;
    var gap = 12;
    // 手机可能比视口还高（320 小屏）：布局以视口内可用高度为准，避免气泡落到屏幕外。
    var availH = Math.min(phoneH, window.innerHeight - pr.top);
    var below = availH - (tr.bottom - pr.top) - gap;
    var above = tr.top - pr.top - gap;
    var place = step.place === 'above' || step.place === 'below' ? step.place : 'below';
    if (!step.place && below < bh + 6 && above > below) place = 'above';
    var bTop = place === 'below' ? (tr.bottom - pr.top + gap) : (tr.top - pr.top - bh - gap);
    if (place === 'below' && bTop + bh > phoneH - 8) {
      if (above >= bh + 8) { place = 'above'; bTop = tr.top - pr.top - bh - gap; }
      else { bTop = phoneH - bh - 8; }
    }
    if (place === 'above' && bTop < 8) {
      if (below >= bh + 8) { place = 'below'; bTop = tr.bottom - pr.top + gap; }
      else { bTop = 8; }
    }
    if (bTop < 8) bTop = 8;
    var maxTop = Math.max(8, availH - bh - 8);
    if (bTop > maxTop) bTop = maxTop;
    var bLeft = (tr.left - pr.left) + tr.width / 2 - bw / 2;
    bLeft = Math.max(10, Math.min(phoneW - bw - 10, bLeft));
    bubbleEl.style.left = bLeft + 'px';
    bubbleEl.style.top = bTop + 'px';
    bubbleEl.setAttribute('data-place', place);
    var tail = (tr.left - pr.left) + tr.width / 2 - bLeft;
    tail = Math.max(20, Math.min(bw - 20, tail));
    bubbleEl.style.setProperty('--tail-x', tail + 'px');
  }

  function renderDots() {
    var dots = bubbleEl.querySelector('.pg-dots');
    dots.innerHTML = '';
    steps.forEach(function (_, i) {
      var d = document.createElement('i');
      d.className = 'pg-dot' + (i === index ? ' is-on' : '');
      dots.appendChild(d);
    });
  }

  function actionOf(step) {
    if (!step || !step.action) return null;
    return typeof step.action === 'string' ? { selector: step.action } : step.action;
  }

  function unbindAction() {
    if (actionCleanup) { actionCleanup(); actionCleanup = null; }
  }

  function bindAction() {
    unbindAction();
    var step = steps[index];
    var action = actionOf(step);
    if (!action) return;
    var target = phoneEl.querySelector(action.selector);
    if (!target) return;
    var evt = action.on || 'click';
    var handler = function (event) {
      if (!rootEl || advancing) return;
      if (evt === 'click' && !target.contains(event.target) && event.target !== target) return;
      if (evt === 'input' && action.minLength && target.value.length < action.minLength) return;
      if (typeof action.filter === 'function' && !action.filter(event, target)) return;
      advancing = true;
      if (action.seen && config && config.store) markSeen(config.store);
      var at = index;
      setTimeout(function () {
        advancing = false;
        if (rootEl && index === at) next();
      }, action.delay || 230);
    };
    target.addEventListener(evt, handler, true);
    actionCleanup = function () { target.removeEventListener(evt, handler, true); };
  }

  function gateOf(step) {
    if (!step || !step.gate) return null;
    return typeof step.gate === 'string' ? { selector: step.gate } : step.gate;
  }

  function paintHint(text) {
    if (!hintEl) return;
    hintEl.hidden = !text;
    hintEl.innerHTML = text ? ico('paw') + '<span>' + text + '</span>' : '';
  }

  function setLocked(lock) {
    locked = !!lock;
    var btn = bubbleEl && bubbleEl.querySelector('.pg-next');
    if (!btn) return;
    btn.disabled = !!lock;
    btn.setAttribute('aria-disabled', lock ? 'true' : 'false');
    btn.classList.toggle('is-locked', !!lock);
  }

  function unbindGate() {
    if (gateCleanup) { gateCleanup(); gateCleanup = null; }
  }

  function bindGate() {
    unbindGate();
    setLocked(false);
    var step = steps[index];
    var gate = gateOf(step);
    if (!gate) return;
    var target = phoneEl.querySelector(gate.selector);
    if (!target) return;
    var evt = gate.on || (target.tagName === 'INPUT' ? 'input' : 'click');
    var unlocked = false;
    function pass(event) {
      if (typeof gate.test === 'function' && !gate.test(event, target)) return false;
      if (gate.minLength && String(target.value || '').length < gate.minLength) return false;
      return true;
    }
    function unlock(event) {
      if (!rootEl || unlocked || !pass(event)) return;
      unlocked = true;
      setLocked(false);
      paintHint(gate.doneHint || step.hint);
      if (gate.focusNext) {
        var b = bubbleEl && bubbleEl.querySelector('.pg-next');
        if (b) { try { b.focus({ preventScroll: true }); } catch (e) {} }
      }
      if (gate.autoNext) {
        var at = index;
        setTimeout(function () {
          if (rootEl && index === at && !locked) next();
        }, gate.autoNext === true ? 420 : gate.autoNext);
      }
    }
    target.addEventListener(evt, unlock, true);
    if (evt !== 'input' && target.tagName === 'INPUT') target.addEventListener('input', unlock, true);
    gateCleanup = function () {
      target.removeEventListener(evt, unlock, true);
      if (evt !== 'input' && target.tagName === 'INPUT') target.removeEventListener('input', unlock, true);
    };
    if (pass({ target: target })) {
      unlocked = true;
      setLocked(false);
      paintHint(gate.doneHint || step.hint);
    } else {
      setLocked(true);
      paintHint(gate.hint || step.hint);
    }
  }

  function prepareStep(step) {
    if (!step || !phoneEl) return;
    try {
      if (typeof step.prepare === 'function') step.prepare(phoneEl);
    } catch (e) {}
    var want = step.state;
    if (want === 'expanded' && phoneEl.__setExpanded) phoneEl.__setExpanded(true);
    if (want === 'collapsed' && phoneEl.__setExpanded) phoneEl.__setExpanded(false);
    if (want === 'expanded' && !phoneEl.__setExpanded) phoneEl.classList.add('expanded');
    if (want === 'collapsed' && !phoneEl.__setExpanded) phoneEl.classList.remove('expanded');
  }

  function showStep(retried) {
    if (!rootEl) return;
    var step = steps[index];
    prepareStep(step);
    var target = step && phoneEl.querySelector(step.target);
    var rect = target && target.getClientRects();
    if (!target || !rect || !rect.length) {
      if (!retried && typeof step.prepare === 'function') {
        setTimeout(function () { showStep(true); }, 360);
        return;
      }
      if (index < steps.length - 1) { index++; showStep(); return; }
      finish(true);
      return;
    }
    bubbleEl.querySelector('.pg-badge').textContent = ('0' + (index + 1)).slice(-2) + ' / ' + ('0' + steps.length).slice(-2);
    bubbleEl.querySelector('.pg-title').textContent = step.title;
    bubbleEl.querySelector('.pg-text').innerHTML = step.text;
    hintEl = bubbleEl.querySelector('.pg-hint');
    paintHint(step.hint);
    bubbleEl.querySelector('.pg-next').innerHTML = (index === steps.length - 1 ? '知道啦' : '下一步') + ' ' + ico('arrow');
    renderDots();
    bindGate();
    layout();
    bubbleEl.classList.remove('is-in');
    void bubbleEl.offsetWidth;
    bubbleEl.classList.add('is-in');
    ensureVisible(target);
    bindAction();
    setTimeout(layout, 80);
    setTimeout(layout, 320);
  }

  function next() {
    if (locked) return;
    if (index < steps.length - 1) {
      index++;
      showStep();
    } else {
      finish(false);
    }
  }

  function finish(skipped) {
    if (config && config.store) markSeen(config.store);
    else if (key) markSeen(key);
    unbindAction();
    unbindGate();
    setLocked(false);
    if (followTimer) { cancelAnimationFrame(followTimer); followTimer = 0; }
    clearTimeout(resizeTimer);
    if (scrollRaf) { cancelAnimationFrame(scrollRaf); scrollRaf = 0; }
    var dying = rootEl;
    rootEl = null; spotEl = null; bubbleEl = null; hintEl = null;
    document.removeEventListener('keydown', onKey, true);
    document.removeEventListener('scroll', onScroll, true);
    window.removeEventListener('resize', onResize);
    window.removeEventListener('orientationchange', onResize);
    if (dying) {
      dying.style.transition = 'opacity .18s ease';
      dying.style.opacity = '0';
      setTimeout(function () { if (dying.parentNode) dying.parentNode.removeChild(dying); }, 190);
    }
    phoneEl = null;
    // Restoring a text field reopens the mobile keyboard after the guide closes.
    if (lastFocus && lastFocus.focus && !/^(INPUT|TEXTAREA|SELECT)$/.test(lastFocus.tagName) && !lastFocus.isContentEditable) {
      try { lastFocus.focus({ preventScroll: true }); } catch (e) {}
    }
    lastFocus = null;
    if (!skipped && config && config.onDone) { try { config.onDone(); } catch (e) {} }
  }

  function onKey(event) {
    if (!rootEl) return;
    if (event.key === 'Escape') { event.preventDefault(); finish(true); return; }
    if (event.key === 'ArrowRight') { event.preventDefault(); next(); return; }
    if (event.key === 'Tab') {
      var f = bubbleEl.querySelectorAll('button');
      if (!f.length) return;
      var first = f[0], last = f[f.length - 1];
      if (event.shiftKey && document.activeElement === first) { event.preventDefault(); last.focus(); }
      else if (!event.shiftKey && document.activeElement === last) { event.preventDefault(); first.focus(); }
    }
  }

  function scheduleLayout() {
    if (!rootEl || scrollRaf) return;
    scrollRaf = requestAnimationFrame(function () {
      scrollRaf = 0;
      layout();
    });
  }

  function onScroll() {
    scheduleLayout();
  }

  function onResize() {
    clearTimeout(resizeTimer);
    resizeTimer = setTimeout(layout, 90);
  }

  function start(which) {
    var cfg = CONFIG[which];
    if (!cfg) return false;
    if (rootEl) return false;
    phoneEl = document.querySelector('.phone');
    if (!phoneEl) return false;
    key = which;
    config = cfg;
    try {
      if (typeof cfg.prepare === 'function') cfg.prepare(phoneEl);
    } catch (e) {}
    steps = (cfg.steps || []).filter(function (s) { return s && s.target; });
    if (!steps.length) return false;
    index = 0;
    lastFocus = document.activeElement;
    rootEl = buildRoot();
    phoneEl.appendChild(rootEl);
    installGuard();
    spotEl = rootEl.querySelector('.pg-spot');
    bubbleEl = rootEl.querySelector('.pg-bubble');
    hintEl = rootEl.querySelector('.pg-hint');

    // 原型页把手机放在页面中间；引导开始时把手机顶端滚到视口内，避免气泡落在屏幕外。
    try {
      var pr = phoneEl.getBoundingClientRect();
      if (pr.top < 0 || pr.bottom > window.innerHeight + 1 || pr.top > 48) {
        window.scrollTo({ top: Math.max(0, window.scrollY + pr.top - 8), behavior: 'smooth' });
      }
    } catch (e) {}

    rootEl.querySelector('.pg-skip').addEventListener('click', function () { finish(true); });
    rootEl.querySelector('.pg-next').addEventListener('click', next);
    document.addEventListener('keydown', onKey, true);
    document.addEventListener('scroll', onScroll, true);
    window.addEventListener('resize', onResize);
    window.addEventListener('orientationchange', onResize);
    showStep();
    setTimeout(function () {
      var b = bubbleEl && bubbleEl.querySelector('.pg-next');
      if (b) b.focus({ preventScroll: true });
    }, 260);
    return true;
  }

  function setKey(which) {
    key = which;
    bindOpeners();
  }

  function bindOpeners() {
    document.querySelectorAll('[data-guide-open]').forEach(function (btn) {
      if (btn.getAttribute('data-pg-bound') === '1') return;
      btn.setAttribute('data-pg-bound', '1');
      btn.addEventListener('click', function (event) {
        event.preventDefault();
        if (key) start(key);
      });
    });
  }

  function auto(which, options) {
    key = which;
    bindOpeners();
    if (params.get('guide') === 'reset') {
      reset();
      setTimeout(function () { if (!rootEl) start(which); }, 60);
      return;
    }
    if (params.get('guide') === 'off' || (options && options.manualOnly)) return;
    var cfg = CONFIG[which];
    if (!cfg) return;
    var forced = params.get('guide') === '1' || params.get('guide') === 'on';
    if (forced || !seen(cfg.store || which)) {
      setTimeout(function () {
        var active = document.activeElement;
        var editing = active && (/^(INPUT|TEXTAREA|SELECT)$/.test(active.tagName) || active.isContentEditable);
        if (!rootEl && !editing) start(which);
      }, forced ? 320 : 760);
    }
  }

  function reset() {
    var blank = { device: state.device, seen: {}, updatedAt: Date.now() };
    writeLocal(blank);
    if (API_BASE) {
      fetch(API_BASE + '/api/guide/reset', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ device: state.device })
      }).catch(function () {});
    }
    return blank;
  }

  window.PandaGuide = {
    auto: auto,
    open: start,
    setKey: setKey,
    setStyle: setStyle,
    style: activeStyle,
    seen: seen,
    reset: reset,
    state: function () { return JSON.parse(JSON.stringify(state)); }
  };

  remoteGet();
})(window, document);