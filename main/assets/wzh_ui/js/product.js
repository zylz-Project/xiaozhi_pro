/* 三款 WZH 玩具 · 产品化层（离线原型）
   同一套页面，根据 ?toy= 或 localStorage 替换产品名、热点、图片、引导头像与产品差异块。
   接固件时把 current()/set() 换成设备信息接口即可。 */
(function (window, document) {
  'use strict';
  var KEY = 'panda-toy';
  var DEFAULT = 'crawlPanda';
  var PRODUCTS = {
    crawlPanda: {
      id: 'crawlPanda', axes: '三轴', name: '爬爬熊猫', short: '熊猫', english: 'CRAWL PANDA', code: 'PANDA',
      ap: 'CrawlPanda', hot: '1A2B', role: '小华', wake: '小华小华', model: 'WZH-CP01',
      theme: 'milk', voice: true,
      assets: { front: 'assets/crawlPanda_正面.webp', side: 'assets/crawlPanda_侧面.webp', head: 'assets/crawlPanda_icon.webp', face: 'assets/crawlPanda_icon.webp', icon: 'assets/crawlPanda_icon.webp' },
      parts: [
        ['头部', '转头、凝视'],
        ['双前肢', '挥爪、扒地、支撑'],
        ['触摸区', '轻触会有回应'],
        ['电源键', '开关机、双击对话'],
        ['USB-C', '充电与烧录'],
        ['扬声器', '对话与本地音效']
      ],
      intro: '一只会转头、会挥爪、会听你说话的桌面小伙伴。',
      motion: '空闲时会轻轻呼吸、凝视、摆动前肢；被触摸、听到声音或对话时，会做出对应动作。',
      specAction: '头部 1 路 + 双前肢 2 路舵机',
      pose: '双爪搭在窗台上',
      action: '转头看你',
      motionActions: ['转头看你','挥挥前爪','扒扒垫子','安静凝视'],
      voiceCopy: '6 个固定音色，也可以复刻家人的声音。',
      resetSub: '清除网络与本机复刻声音'
    },
    dinosaur: {
      id: 'dinosaur', axes: '五轴', name: '小恐龙', short: '恐龙', english: 'LITTLE DINO', code: 'DINO',
      ap: 'Dino', hot: '6C3F', role: '小威', wake: '小威小威', model: 'WZH-DN01',
      theme: 'bamboo', voice: true,
      assets: { front: 'assets/dinosaur_front.webp', side: 'assets/dinosaur_side.webp', head: 'assets/dinosaur_icon.webp', face: 'assets/dinosaur_icon.webp', icon: 'assets/dinosaur_icon.webp' },
      parts: [
        ['头部', '抬头、转头、凝视'],
        ['颈部', '前倾、后仰、左右倾'],
        ['尾巴', '上下、左右摆动'],
        ['触摸区', '轻触会有回应'],
        ['电源键', '开关机、双击对话'],
        ['USB-C', '充电与烧录']
      ],
      intro: '一只会伸脖子、会仰天吼叫、会摇尾巴的长颈小恐龙。',
      motion: '五轴关节会跟着声音和触摸动起来：抬头看你、前倾取食、仰天吼叫，尾巴慢慢摆。',
      specAction: '颈部 2 路 + 头部 1 路 + 尾巴 2 路舵机',
      pose: '长脖子探出窗台',
      action: '仰天吼叫',
      motionActions: ['抬头看你','前倾取食','仰天吼叫','尾巴慢摆'],
      voiceCopy: '6 个固定音色，也可以复刻家人的声音。',
      resetSub: '清除网络与本机复刻声音'
    },
    redpanda: {
      id: 'redpanda', axes: '三轴', name: '摇尾小熊猫', short: '小熊猫', english: 'TAIL RED PANDA', code: 'RED PANDA',
      ap: 'RedPanda', hot: '9D2E', role: '小武', wake: '小武小武', model: 'WZH-RP01',
      theme: 'berry', voice: true,
      assets: { front: 'assets/tailRedPanda_front.webp', side: 'assets/tailRedPanda_side.webp', head: 'assets/tailRedPanda_icon.webp', face: 'assets/tailRedPanda_icon.webp', icon: 'assets/tailRedPanda_icon.webp' },
      parts: [
        ['头部', '转头、凝视'],
        ['大尾巴', '左右摇、上下翘'],
        ['触摸区', '轻触会有回应'],
        ['电源键', '开关机、双击对话'],
        ['USB-C', '充电与烧录'],
        ['扬声器', '对话与本地音效']
      ],
      intro: '一只会转头、会摇大尾巴、会听你说话的桌面小伙伴。',
      motion: '三轴关节让大尾巴连续画弧：开心时高高翘起，听到声音时左右轻摆，安静时停在半空。',
      specAction: '头部 1 路 + 尾巴 2 路舵机',
      pose: '大尾巴搭在坐垫上',
      action: '摇摇大尾巴',
      motionActions: ['转头看你','尾巴画弧','高高翘起','安静停住'],
      voiceCopy: '6 个固定音色，也可以复刻家人的声音。',
      resetSub: '清除网络与本机复刻声音'
    }
  };

  function param(name) {
    try { return new URLSearchParams(window.location.search).get(name); } catch (e) { return null; }
  }

  var ALIASES = { panda: 'crawlPanda', crawlPanda: 'crawlPanda', dinosaur: 'dinosaur', redpanda: 'redpanda', tailRedPanda: 'redpanda', wzh_tailRedPanda: 'redpanda' };

  function pick() {
    var raw = param('toy');
    var asked = ALIASES[raw] || raw;
    if (asked && PRODUCTS[asked]) {
      try { localStorage.setItem(KEY, asked); } catch (e) {}
      return asked;
    }
    try {
      var rawSaved = localStorage.getItem(KEY);
      var saved = ALIASES[rawSaved] || rawSaved;
      if (saved && PRODUCTS[saved]) return saved;
    } catch (e) {}
    return DEFAULT;
  }

  var currentId = pick();
  var P = PRODUCTS[currentId];
  P.id = currentId;
  var titleTemplate = document.title;
  var deviceStatus = null;

  function each(sel, fn) {
    Array.prototype.forEach.call(document.querySelectorAll(sel), fn);
  }

  function text(sel, value) {
    each(sel, function (el) { el.textContent = value; });
  }

  function setImage(el, src) {
    if (!el || !src) return;
    if (el.tagName === 'IMG') el.src = src;
    else {
      var img = el.querySelector('img');
      if (img) img.src = src;
    }
  }

  function apply() {
    try {
      if (window.PandaTheme && !localStorage.getItem(PandaTheme.key)) {
        PandaTheme.set({ theme: P.theme });
      }
    } catch (e) {}
    document.documentElement.setAttribute('data-toy', P.id);
    if (document.body) document.body.setAttribute('data-toy', P.id);
    each('.phone', function (el) { el.setAttribute('data-toy', P.id); });

    text('[data-p-name]', P.name);
    text('[data-p-short]', P.short);
    text('[data-p-en]', P.english);
    text('[data-p-code]', P.code);
    text('[data-p-role]', P.role);
    text('[data-p-wake]', P.wake);
    text('[data-p-model]', P.model);
    text('[data-p-ap]', P.ap + '-XXXX');
    text('[data-p-hot]', P.ap + '-XXXX');
    text('[data-p-pass]', 'zylz + 热点名称后四位');
    text('[data-p-version]', '--');
    text('[data-p-intro]', P.intro);
    text('[data-p-motion]', P.motion);
    text('[data-p-spec-action]', P.specAction);
    text('[data-p-pose]', P.pose);
    text('[data-p-action]', P.action);
    text('[data-p-voice-copy]', P.voiceCopy);
    text('[data-p-reset-sub]', P.resetSub);
    text('[data-p-axes]', P.axes);
    var ap = deviceStatus && deviceStatus.wifi && deviceStatus.wifi.ap_ssid;
    if (ap) {
      text('[data-p-ap], [data-p-hot]', ap);
      text('[data-p-pass]', 'zylz' + ap.slice(-4));
    }
    if (deviceStatus) text('[data-p-version]', deviceStatus.version || '--');
    (P.motionActions || []).forEach(function (v, i) { each('[data-p-action-' + (i + 1) + ']', function (el) { el.textContent = v; }); });
    text('[data-p-sound-title]', P.voice ? '声音' : '动作');
    text('[data-p-sound-meta]', P.voice ? '音色管理' : P.axes + '关节 · 动作说明');

    each('[data-p-img]', function (el) {
      var key = el.getAttribute('data-p-img');
      setImage(el, P.assets[key]);
    });

    each('[data-only]', function (el) {
      var v = el.getAttribute('data-only');
      var hide = v !== P.id && !(v === 'panda' && P.id === 'crawlPanda') && !(v === 'other' && P.id !== 'crawlPanda');
      el.hidden = hide;
      if (hide) el.style.display = 'none'; else el.style.removeProperty('display');
    });
    each('[data-p-block]', function (el) {
      var show = el.getAttribute('data-p-block') === P.id;
      el.hidden = !show;
      if (!show) el.style.display = 'none'; else el.style.removeProperty('display');
    });
    each('[data-except]', function (el) {
      var hide2 = el.getAttribute('data-except') === P.id;
      el.hidden = hide2;
      if (hide2) el.style.display = 'none'; else el.style.removeProperty('display');
    });

    document.title = titleTemplate.replace(/爬爬熊猫/g, P.name).replace(/CrawlPanda/g, P.ap);

    var setupTitle = document.getElementById('setupTitle');
    if (setupTitle) setupTitle.textContent = '连接' + P.name;
    var hotspot = document.getElementById('hotspotName');
    if (hotspot) hotspot.textContent = ap || P.ap + '-XXXX';
    each('.pg-who', function (el) { el.textContent = P.name + ' · 使用引导'; });

    if (P.id !== 'crawlPanda') {
      each('.pg-root', function (el) { el.setAttribute('aria-label', P.name + '使用引导'); });
    }
  }

  function set(id, go) {
    if (!PRODUCTS[id]) return;
    try { localStorage.setItem(KEY, id); } catch (e) {}
    if (!go) {
      var changed = currentId !== id;
      currentId = id; P = PRODUCTS[id]; P.id = id;
      window.PandaProduct.id = id;
      window.PandaProduct.data = P;
      apply();
      if (changed) document.dispatchEvent(new CustomEvent('panda:product', { detail: P }));
      return;
    }
    try {
      if (window.PandaTheme) {
        var st = window.PandaTheme.get();
        window.PandaTheme.set({ theme: PRODUCTS[id].theme || st.theme });
      }
    } catch (e) {}
    var url = new URL(window.location.href);
    url.searchParams.set('toy', id);
    window.location.href = url.toString();
  }

  window.PandaProduct = {
    data: P,
    id: P.id,
    products: PRODUCTS,
    get: function () { return P; },
    set: set,
    apply: apply,
    key: KEY
  };

  fetch('/api/status', {cache:'no-store'}).then(function(r){ return r.json(); }).then(function(s){
    var id = {'wzh-dinosaur': 'dinosaur', 'wzh-tailredpanda': 'redpanda', 'wzh-crawlpanda': 'crawlPanda'}[s.build];
    if (!id) return;
    deviceStatus = s;
    var applyDevice = function () {
      set(id, false);
    };
    if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', applyDevice);
    else applyDevice();
  }).catch(function(){});

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', apply);
  } else {
    apply();
  }
})(window, document);
