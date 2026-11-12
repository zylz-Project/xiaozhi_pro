(function () {
  var states = {
    idle: { status: '在线', sheet: '待机', cta: '开始对话', disabled: false, more: 'Home · 2.4G', net: 'Home · 2.4G', netName: 'Home', netSub: '2.4G · 满格', latency: '18ms' },
    listening: { status: '正在听', sheet: '聆听中', cta: '结束对话', disabled: false, more: 'Home · 2.4G', net: 'Home · 2.4G', netName: 'Home', netSub: '2.4G · 满格', latency: '18ms' },
    speaking: { status: '正在说', sheet: '说话中', cta: '结束对话', disabled: false, more: 'Home · 2.4G', net: 'Home · 2.4G', netName: 'Home', netSub: '2.4G · 满格', latency: '18ms' },
    connecting: { status: '连接中', sheet: '连接中', cta: '连接中…', disabled: true, more: '正在连接 · 2.4G', net: 'Home · 2.4G', netName: 'Home', netSub: '2.4G · 连接中', latency: '…' },
    offline: { status: '离线', sheet: '离线', cta: '设备离线', disabled: true, more: '离线 · 上次 Home', net: '上次 Home · 2.4G', netName: '上次 Home', netSub: '上次 2.4G · 离线', latency: '—' }
  };
  var phones = Array.prototype.slice.call(document.querySelectorAll('.phone'));
  var buttons = Array.prototype.slice.call(document.querySelectorAll('.statebar button'));

  function setState(state) {
    var cfg = states[state] || states.idle;
    phones.forEach(function (phone) {
      phone.setAttribute('data-state', state);
      phone.querySelectorAll('.status-text').forEach(function (el) { el.textContent = cfg.status; });
      phone.querySelectorAll('.sheet-state').forEach(function (el) { el.textContent = cfg.sheet; });
      phone.querySelectorAll('.more-state').forEach(function (el) { el.textContent = cfg.more; });
      phone.querySelectorAll('.net-val').forEach(function (el) { el.textContent = cfg.net; });
      phone.querySelectorAll('.net-name').forEach(function (el) { el.textContent = cfg.netName; });
      phone.querySelectorAll('.net-sub').forEach(function (el) { el.textContent = cfg.netSub; });
      phone.querySelectorAll('.net-chip').forEach(function (el) { el.textContent = cfg.latency; });
      var metricNet = phone.querySelector('.metric-net');
      if (metricNet) {
        metricNet.classList.toggle('is-offline', state === 'offline');
        metricNet.classList.toggle('is-connecting', state === 'connecting');
      }
      phone.querySelectorAll('[data-metric-net]').forEach(function (el) {
        el.textContent = state === 'offline' ? '上次 Home' : 'Home';
      });
      phone.querySelectorAll('[data-metric-signal]').forEach(function (el) {
        el.textContent = state === 'offline' ? '离线' : state === 'connecting' ? '连接中' : '强';
      });
      var cta = phone.querySelector('.primary');
      var label = phone.querySelector('.cta-label');
      label.textContent = cfg.cta;
      cta.disabled = cfg.disabled;
      cta.setAttribute('aria-disabled', cfg.disabled ? 'true' : 'false');
    });
    buttons.forEach(function (btn) {
      btn.classList.toggle('on', btn.getAttribute('data-state') === state);
    });
  }

  buttons.forEach(function (btn) {
    btn.addEventListener('click', function () { setState(btn.getAttribute('data-state')); });
  });

  phones.forEach(function (phone) {
    var handle = phone.querySelector('.handle');
    var scrim = phone.querySelector('.scrim');
    var primary = phone.querySelector('.primary');
    var manualBtn = phone.querySelector('.con-manual');
    var manualSheet = phone.querySelector('.manual-sheet');
    var manualFrame = phone.querySelector('.manual-frame');
    var manualClose = phone.querySelector('.manual-close');
    var talkBtn = phone.querySelector('.talk-cta');
    var closeBtn = phone.querySelector('.con-close');

    function setManual(next) {
      phone.classList.toggle('manual-open', next);
      if (manualBtn) manualBtn.setAttribute('aria-expanded', next ? 'true' : 'false');
      if (manualSheet) manualSheet.setAttribute('aria-hidden', next ? 'false' : 'true');
      if (next && manualFrame && window.PandaTheme) {
        var st = window.PandaTheme.get();
        var toy = window.PandaProduct ? window.PandaProduct.get().id : '';
        var src = '/manual?embed=1&theme=' + encodeURIComponent(st.theme) + '&mode=' + encodeURIComponent(st.mode)
          + (toy ? '&toy=' + encodeURIComponent(toy) : '');
        if (manualFrame.getAttribute('src') !== src) manualFrame.src = src;
      }
    }

    function setExpanded(next) {
      phone.classList.toggle('expanded', next);
      handle.setAttribute('aria-expanded', next ? 'true' : 'false');
      handle.setAttribute('aria-label', next ? '收起控制台' : '展开控制台');
      if (!next) setManual(false);
    }
    phone.__setExpanded = setExpanded;
    phone.__setManual = setManual;

    function toggleConversation() {
      var state = phone.getAttribute('data-state');
      if (state === 'idle') setState('speaking');
      else if (state === 'speaking' || state === 'listening') setState('idle');
    }

    handle.addEventListener('click', function () {
      setExpanded(!phone.classList.contains('expanded'));
    });
    scrim.addEventListener('click', function () {
      if (phone.classList.contains('manual-open')) setManual(false);
      else setExpanded(false);
    });

    primary.addEventListener('click', toggleConversation);

    var voiceOpen = phone.querySelector('.voice-row .voice-open');
    var voiceTry = phone.querySelector('.voice-row .voice-try');
    var voiceAlt = phone.querySelector('.voice-alt');
    if (voiceAlt) voiceAlt.addEventListener('click', function () {
      setExpanded(true);
      var more = phone.querySelector('.sheet-more');
      var sound = phone.querySelector('.panel-sound');
      if (more && sound) setTimeout(function () { more.scrollTo({ top: Math.max(0, sound.offsetTop - 8), behavior: 'smooth' }); }, 380);
    });
    var actionRow = phone.querySelector('.sheet-action-row');
    if (actionRow) actionRow.addEventListener('click', function () {
      setExpanded(true);
      var more = phone.querySelector('.sheet-more');
      var sound = phone.querySelector('.panel-sound');
      if (more && sound) setTimeout(function () { more.scrollTo({ top: Math.max(0, sound.offsetTop - 8), behavior: 'smooth' }); }, 380);
    });
    if (voiceOpen) voiceOpen.addEventListener('click', function () {
      setExpanded(true);
      var scroll = phone.querySelector('.console-scroll') || phone.querySelector('.sheet-more');
      var sound = phone.querySelector('.panel-sound');
      if (scroll && sound) {
        setTimeout(function () {
          var sr = scroll.getBoundingClientRect();
          var panelRect = sound.getBoundingClientRect();
          var panelTop = panelRect.top - sr.top + scroll.scrollTop;
          var panelBottom = panelRect.bottom - sr.top + scroll.scrollTop;
          var max = Math.max(0, scroll.scrollHeight - scroll.clientHeight);
          var target = panelBottom > scroll.clientHeight + 8
            ? Math.min(max, panelBottom - scroll.clientHeight + 16)
            : Math.min(panelTop - 4, max);
          scroll.scrollTo({ top: Math.max(0, target), behavior: 'smooth' });
        }, 380);
      }
    });
    if (voiceTry) voiceTry.addEventListener('click', function () {
      var nameEl = phone.querySelector('.voice-row .voice-meta b');
      var name = nameEl ? nameEl.textContent : '当前音色';
      toast('试听 · ' + name);
      voiceTry.classList.add('is-playing');
      clearTimeout(voiceTry._playing);
      voiceTry._playing = setTimeout(function () { voiceTry.classList.remove('is-playing'); }, 1600);
    });

    if (talkBtn) talkBtn.addEventListener('click', function () {
      var state = phone.getAttribute('data-state');
      if (state === 'offline') { toast('设备离线 · 连接后可对话'); return; }
      if (state === 'connecting') { toast('正在连接 · 请稍候'); return; }
      toggleConversation();
    });
    if (closeBtn) closeBtn.addEventListener('click', function () { setExpanded(false); });
    if (manualBtn) manualBtn.addEventListener('click', function () { setExpanded(true); setManual(true); });
    if (manualClose) manualClose.addEventListener('click', function () { setManual(false); });

    var prefKey = 'panda';
    var themeNames = { milk: '奶糖', salt: '海盐', berry: '莓果', bamboo: '竹青' };

    function toast(message) {
      var el = phone.querySelector('.toast');
      if (!el) {
        el = document.createElement('div');
        el.className = 'toast';
        el.setAttribute('role', 'status');
        el.setAttribute('aria-live', 'polite');
        phone.appendChild(el);
      }
      el.textContent = message;
      el.classList.add('show');
      clearTimeout(el._timer);
      el._timer = setTimeout(function () { el.classList.remove('show'); }, 2100);
    }

    function updateThemeMeta() {
      var label = phone.querySelector('.theme-now');
      if (!label) return;
      var material = phone.classList.contains('b') ? '奶润' : '通透';
      label.textContent = (themeNames[phone.dataset.theme] || '奶糖') + ' · ' + material;
    }

    function setTheme(theme, silent) {
      phone.dataset.theme = theme;
      phone.querySelectorAll('[data-theme]').forEach(function (other) {
        other.setAttribute('aria-pressed', other.getAttribute('data-theme') === theme ? 'true' : 'false');
      });
      updateThemeMeta();
      if (!silent && window.PandaTheme) window.PandaTheme.set({ theme: theme });
    }

    function setMaterial(mat, silent) {
      phone.classList.toggle('a', mat === 'a');
      phone.classList.toggle('b', mat !== 'a');
      phone.querySelectorAll('[data-mat]').forEach(function (other) {
        other.setAttribute('aria-pressed', other.getAttribute('data-mat') === mat ? 'true' : 'false');
      });
      updateThemeMeta();
      if (!silent && window.PandaTheme) window.PandaTheme.set({ material: mat });
    }

    function setLayout(layout, announce, silent) {
      var consoleEl = phone.querySelector('.console');
      if (!consoleEl) return;
      consoleEl.setAttribute('data-layout', layout);
      phone.setAttribute('data-layout', layout);
      phone.querySelectorAll('.layout-btn').forEach(function (btn) {
        btn.setAttribute('aria-pressed', btn.getAttribute('data-layout') === layout ? 'true' : 'false');
      });
      var phoneLabel = phone.parentElement ? phone.parentElement.querySelector('.phone-label b') : null;
      if (phoneLabel) phoneLabel.textContent = layout === 'c' ? '连续控制台' : '卡片控制台';
      if (announce) toast(layout === 'c' ? '布局 · 连续面板' : '布局 · 卡片面板');
      if (!silent && window.PandaTheme) window.PandaTheme.set({ layout: layout });
    }

    phone.querySelectorAll('[data-theme]').forEach(function (btn) {
      btn.addEventListener('click', function () {
        var theme = btn.getAttribute('data-theme');
        setTheme(theme);
      });
    });

    phone.querySelectorAll('[data-mat]').forEach(function (btn) {
      btn.addEventListener('click', function () {
        var mat = btn.getAttribute('data-mat');
        setMaterial(mat);
      });
    });

    phone.querySelectorAll('.layout-btn').forEach(function (btn) {
      btn.addEventListener('click', function () {
        var layout = btn.getAttribute('data-layout');
        setLayout(layout, true);
      });
    });

    // Restore the shared appearance after the controls exist.
    try {
      var saved = window.PandaTheme ? window.PandaTheme.get() : { theme: 'milk', material: 'b', layout: 'c' };
      setTheme(saved.theme, true);
      setMaterial(saved.material, true);
      setLayout(saved.layout || (phone.querySelector('.console').getAttribute('data-layout') || 'c'), false, true);
    } catch (e) {
      setLayout(phone.querySelector('.console').getAttribute('data-layout') || 'c', false, true);
    }
    updateThemeMeta();

    phone.querySelectorAll('.manual-ribbon').forEach(function (btn) {
      btn.addEventListener('click', function () { toast('使用说明书 · 图文版，接入后打开'); });
    });
    phone.querySelectorAll('.net-change').forEach(function (btn) {
      btn.addEventListener('click', function () { location.href = '/setup?from=device'; });
    });
    phone.querySelectorAll('.clone-manage').forEach(function (btn) {
      btn.addEventListener('click', function () {
        toast('打开音色管理（三种方案）');
        window.open('voice-manager.html', '_blank', 'noopener');
      });
    });

    function voiceInitial(name) {
      var clean = String(name || '').replace(/\.[a-z0-9]+$/i, '').trim();
      var chars = Array.from(clean);
      if (!chars.length) return '声';
      var cjk = chars.filter(function (ch) { return /[\u3400-\u9fff]/.test(ch); });
      if (cjk.length) return cjk[cjk.length - 1];
      return chars[0].toUpperCase();
    }

    function selectVoice(source) {
      var name = typeof source === 'string' ? source : (source.getAttribute('data-voice') || source.textContent.trim());
      phone.querySelectorAll('.voice-chip').forEach(function (other) {
        other.classList.toggle('on', other.getAttribute('data-voice') === name);
      });
      var now = phone.querySelector('.voice-now');
      if (now) now.textContent = name;
      var mini = phone.querySelector('.voice-row .voice-meta b');
      if (mini) mini.textContent = name;
      var avatar = phone.querySelector('.voice-row [data-voice-avatar]');
      if (avatar) avatar.textContent = voiceInitial(name);
      var open = phone.querySelector('.voice-row .voice-open');
      if (open) open.setAttribute('aria-label', '当前音色：' + name + '，打开声音设置');
      var managerCurrent = phone.querySelector('[data-vm-current]');
      if (managerCurrent) managerCurrent.textContent = name;
      var managerAvatar = phone.querySelector('[data-vm-current-avatar]');
      if (managerAvatar) managerAvatar.textContent = voiceInitial(name);
      phone.querySelectorAll('.vm-row').forEach(function (vmRow) {
        vmRow.classList.toggle('on', vmRow.getAttribute('data-name') === name);
      });
      phone.querySelectorAll('.vm-use').forEach(function (useBtn) {
        var vmRow = useBtn.closest('.vm-row');
        useBtn.textContent = vmRow && vmRow.classList.contains('on') ? '使用中' : '使用';
      });
      phone.setAttribute('data-current-voice', name);
    }
    phone.__selectVoice = selectVoice;
    phone.__toast = toast;
    phone.querySelectorAll('.voice-chip').forEach(function (btn) {
      btn.addEventListener('click', function () { selectVoice(btn); });
    });
    var listenMini = phone.querySelector('.listen-mini');
    var nowVoice = phone.querySelector('.now-voice');
    if (listenMini && nowVoice) {
      listenMini.addEventListener('click', function () {
        nowVoice.classList.add('auditioning');
        var current = phone.querySelector('.voice-now');
        toast('试听 · ' + (current ? current.textContent : '当前音色'));
        clearTimeout(nowVoice._audition);
        nowVoice._audition = setTimeout(function () { nowVoice.classList.remove('auditioning'); }, 1600);
      });
    }

    var lab = phone.querySelector('.clone-lab');
    if (lab) {
      var rec = lab.querySelector('.lab-rec');
      var labTitle = lab.querySelector('.lab-title');
      var labSub = lab.querySelector('.lab-sub');
      var recTimer = null;
      var recResetTimer = null;
      var recSeconds = 0;

      function finishRecording() {
        if (!lab.classList.contains('recording')) return;
        lab.classList.remove('recording');
        if (rec) rec.setAttribute('aria-pressed', 'false');
        clearInterval(recTimer);
        recTimer = null;
        labTitle.textContent = '已录 ' + recSeconds + ' 秒';
        labSub.textContent = '下一步：生成专属音色';
        toast('录音结束 · 生成中（原型）');
        clearTimeout(recResetTimer);
        recResetTimer = setTimeout(function () {
          if (!lab.classList.contains('recording')) {
            labTitle.textContent = '复刻声音';
            labSub.textContent = '录音或上传，生成音色';
          }
        }, 2400);
      }


      var uploadBtn = lab.querySelector('.lab-upload');
      var fileInput = lab.querySelector('.lab-file');
      var rail = phone.querySelector('.voice-rail[aria-label="我的复刻音色"]');

      function updateLabCount() {
        var count = phone.querySelectorAll('.clone-chip').length;
        var labCount = phone.querySelector('.lab-count');
        if (labCount) labCount.textContent = count + ' / 10';
        var meta = phone.querySelector('.panel-sound .panel-meta');
        if (meta) meta.textContent = '固定 6 · 复刻 ' + count;
      }

      function addCloneChip(name) {
        if (!rail) return;
        var chip = document.createElement('button');
        chip.type = 'button';
        chip.className = 'voice-chip clone-chip';
        chip.setAttribute('data-voice', name);
        chip.setAttribute('aria-label', '选择复刻音色 ' + name);
        var face = document.createElement('span');
        face.className = 'chip-face';
        face.textContent = name.slice(-1);
        chip.appendChild(face);
        chip.appendChild(document.createTextNode(name));
        chip.addEventListener('click', function () { selectVoice(chip); });
        var add = rail.querySelector('[data-voice-add]');
        rail.insertBefore(chip, add);
        updateLabCount();
        selectVoice(chip);
      }

      if (uploadBtn && fileInput) {
        uploadBtn.addEventListener('click', function () { fileInput.click(); });
        fileInput.addEventListener('change', function () {
          var file = fileInput.files && fileInput.files[0];
          if (!file) return;
          var base = file.name.replace(/\.[^.]+$/, '').slice(0, 8) || '新音色';
          labTitle.textContent = '正在生成';
          labSub.textContent = file.name;
          toast('已选择 · ' + base);
          clearTimeout(recResetTimer);
          recResetTimer = setTimeout(function () {
            labTitle.textContent = '复刻声音';
            labSub.textContent = '录音或上传，生成音色';
            addCloneChip(base);
            toast('复刻完成 · ' + base + '（原型）');
          }, 1800);
          fileInput.value = '';
        });
      }
    }

    // A warm little assistant bubble, draggable only while the console is open.
    function mountAssistant() {
      var btn = document.createElement('button');
      btn.type = 'button';
      btn.className = 'float-assistant';
      btn.setAttribute('aria-label', '小助手：拖动可移动，单击开始对话');
      var faceSrc = window.PandaProduct ? PandaProduct.get().assets.face : 'assets/crawlPanda_icon.webp';
      var face = '<img src="' + faceSrc + '" alt="">';
      btn.innerHTML = '<span class="fa-pulse"></span><span class="fa-face">' + face + '</span>' +
        '<span class="fa-mic"><svg class="ico" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round"><use href="#i-mic"></use></svg></span>' +
        '';
      phone.appendChild(btn);
      var drag = { active: false, moved: false, startX: 0, startY: 0, startLeft: 0, startTop: 0 };
      var suppressClick = false;
      function beginDrag(event) {
        if (event.button !== undefined && event.button !== 0) return;
        var phoneRect = phone.getBoundingClientRect();
        var btnRect = btn.getBoundingClientRect();
        drag.active = true;
        drag.moved = false;
        drag.startX = event.clientX;
        drag.startY = event.clientY;
        drag.startLeft = btnRect.left - phoneRect.left;
        drag.startTop = btnRect.top - phoneRect.top;
        btn.style.left = drag.startLeft + 'px';
        btn.style.top = drag.startTop + 'px';
        btn.style.right = 'auto';
        btn.style.bottom = 'auto';
        btn.classList.add('dragging');
        try { btn.setPointerCapture(event.pointerId); } catch (err) {}
      }
      function moveDrag(event) {
        if (!drag.active) return;
        var dx = event.clientX - drag.startX;
        var dy = event.clientY - drag.startY;
        if (Math.abs(dx) > 4 || Math.abs(dy) > 4) drag.moved = true;
        var size = btn.offsetWidth || 64;
        var maxX = phone.clientWidth - size - 8;
        var maxY = phone.clientHeight - size - 8;
        var nextX = Math.max(8, Math.min(maxX, drag.startLeft + dx));
        var nextY = Math.max(8, Math.min(maxY, drag.startTop + dy));
        btn.style.left = nextX + 'px';
        btn.style.top = nextY + 'px';
        event.preventDefault();
      }
      function endDrag(event) {
        if (!drag.active) return;
        drag.active = false;
        btn.classList.remove('dragging');
        try { btn.releasePointerCapture(event.pointerId); } catch (err) {}
        if (drag.moved) suppressClick = true;
      }
      btn.addEventListener('pointerdown', beginDrag);
      btn.addEventListener('pointermove', moveDrag);
      btn.addEventListener('pointerup', endDrag);
      btn.addEventListener('pointercancel', endDrag);
      btn.addEventListener('click', function (event) {
        if (suppressClick) { suppressClick = false; event.preventDefault(); return; }
        var state = phone.getAttribute('data-state');
        if (state === 'offline') { toast('设备离线 · 连接后可对话'); return; }
        if (state === 'connecting') { toast('正在连接 · 请稍候'); return; }
        toggleConversation();
      });
    }

    mountAssistant();
  });

  document.addEventListener('keydown', function (event) {
    if (event.key === 'Escape') {
      phones.forEach(function (phone) {
        if (phone.classList.contains('manual-open')) {
          phone.classList.remove('manual-open');
          var m = phone.querySelector('.con-manual');
          var ms = phone.querySelector('.manual-sheet');
          if (m) m.setAttribute('aria-expanded', 'false');
          if (ms) ms.setAttribute('aria-hidden', 'true');
        } else {
          phone.classList.remove('expanded');
          var h = phone.querySelector('.handle');
          if (h) { h.setAttribute('aria-expanded', 'false'); h.setAttribute('aria-label', '展开控制台'); }
        }
      });
    }
  });

  document.querySelectorAll('.volume').forEach(function (input) {
    function paint() {
      input.style.setProperty('--val', input.value + '%');
      var val = input.parentNode.querySelector('.volume-val');
      if (val) val.textContent = input.value;
    }
    input.addEventListener('input', paint);
    paint();
  });

  setState('idle');
})();

  (function () {
    var phone = document.querySelector('.phone');
    if (!phone) return;
    var selectVoice = phone.__selectVoice;
    var toast = phone.__toast || function () {};
    function $(sel, root) { return (root || phone).querySelector(sel); }
    function $$(sel, root) { return Array.prototype.slice.call((root || phone).querySelectorAll(sel)); }

    function openModal(el) {
      if (!el) return;
      el.classList.add('show');
      el.setAttribute('aria-hidden', 'false');
    }
    function closeModal(el) {
      if (!el) return;
      el.classList.remove('show');
      el.setAttribute('aria-hidden', 'true');
    }

    var voiceModal = $('[data-voice-modal]');
    var audioModal = $('[data-audio-modal]');
    var resetModal = $('[data-reset-modal]');
    var vmList = $('[data-vm-list]');
    var vmConfirm = $('[data-vm-confirm]');
    var pendingDelete = null;
    var audioFromManager = false;

    function cloneChips() { return $$('.clone-chip'); }
    function updateCapacity() {
      var count = cloneChips().length;
      var labCount = $('.lab-count');
      if (labCount) labCount.textContent = count + ' / 10';
      var meta = $('.panel-sound .panel-meta');
      if (meta) meta.textContent = '固定 6 · 复刻 ' + count;
      var vmCount = $('[data-vm-count]');
      if (vmCount) vmCount.textContent = count;
      var badge = $('[data-manage-count]');
      if (badge) badge.textContent = count;
    }

    function chipTemplate(name) {
      var chip = document.createElement('button');
      chip.type = 'button';
      chip.className = 'voice-chip clone-chip';
      chip.setAttribute('data-voice', name);
      chip.setAttribute('aria-label', '选择复刻音色 ' + name);
      var face = document.createElement('span');
      face.className = 'chip-face';
      face.textContent = name.slice(-1);
      chip.appendChild(face);
      chip.appendChild(document.createTextNode(name));
      chip.addEventListener('click', function () { selectVoice(name); });
      return chip;
    }

    function rowTemplate(name, date) {
      var row = document.createElement('div');
      row.className = 'vm-row';
      row.setAttribute('data-name', name);
      row.setAttribute('data-date', date);
      row.innerHTML = '<span class="vm-row-avatar">' + name.slice(-1) + '</span>' +
        '<div class="vm-row-copy"><b></b><small>复刻音色 · ' + date + '</small></div>' +
        '<div class="vm-row-actions"><button class="vm-use" type="button" data-use>使用</button>' +
        '<button class="vm-icon" type="button" data-rename aria-label="重命名"><svg class="ico" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><use href="#i-edit"></use></svg></button>' +
        '<button class="vm-icon" type="button" data-delete aria-label="删除"><svg class="ico" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><use href="#i-trash"></use></svg></button></div>';
      row.querySelector('.vm-row-copy b').textContent = name;
      return row;
    }

    function addVoice(name) {
      if (!name) return;
      var rail = $('.clone-rail');
      var addBtn = $('[data-voice-add]');
      if (rail && addBtn) rail.insertBefore(chipTemplate(name), addBtn);
      if (vmList) {
        var row = rowTemplate(name, String(new Date().getMonth() + 1).padStart(2, '0') + '-' + String(new Date().getDate()).padStart(2, '0'));
        vmList.appendChild(row);
      }
      updateCapacity();
      selectVoice(name);
    }

    function openVoiceManager() {
      if (!voiceModal) return;
      updateCapacity();
      openModal(voiceModal);
    }
    function closeVoiceManager() {
      closeModal(voiceModal);
      if (vmConfirm) vmConfirm.classList.remove('show', 'show-error');
      pendingDelete = null;
    }

    $$('[data-open-voice-manager]').forEach(function (btn) {
      btn.addEventListener('click', function (event) {
        event.preventDefault();
        openVoiceManager();
      });
    });
    $$('[data-voice-add]').forEach(function (btn) {
      btn.addEventListener('click', function (event) {
        event.preventDefault();
        openAudio(false, 'record');
      });
    });
    // 录音卡只负责打开「新建复刻音色」工作区，不直接开始录音。
    $$('.lab-record').forEach(function (card) {
      card.addEventListener('click', function (event) {
        event.preventDefault();
        openAudio(false, 'record');
      });
    });
    $$('[data-close-voice]').forEach(function (btn) { btn.addEventListener('click', closeVoiceManager); });
    if (voiceModal) {
      voiceModal.addEventListener('click', function (event) {
        if (event.target === voiceModal) closeVoiceManager();
      });
    }
    if (vmConfirm) {
      vmConfirm.addEventListener('click', function (event) {
        if (event.target === vmConfirm) {
          vmConfirm.classList.remove('show', 'show-error');
          pendingDelete = null;
        }
      });
    }
    $$('[data-vm-cancel]').forEach(function (btn) {
      btn.addEventListener('click', function () {
        if (vmConfirm) vmConfirm.classList.remove('show', 'show-error');
        pendingDelete = null;
      });
    });

    function beginRename(row) {
      var title = row.querySelector('.vm-row-copy b');
      if (!title || row.querySelector('input')) return;
      var oldName = row.getAttribute('data-name');
      var input = document.createElement('input');
      input.className = 'vm-rename-input';
      input.value = oldName;
      input.setAttribute('maxlength', '8');
      title.replaceWith(input);
      input.focus();
      input.select();
      var done = false;
      function finish(save) {
        if (done) return;
        done = true;
        var next = input.value.trim().slice(0, 8);
        if (!save || !next) next = oldName;
        var b = document.createElement('b');
        b.textContent = next;
        input.replaceWith(b);
        if (next === oldName) return;
        row.setAttribute('data-name', next);
        var chip = cloneChips().filter(function (c) { return c.getAttribute('data-voice') === oldName; })[0];
        if (chip) {
          chip.setAttribute('data-voice', next);
          chip.setAttribute('aria-label', '选择复刻音色 ' + next);
          var face = chip.querySelector('.chip-face');
          if (face) face.textContent = next.slice(-1);
          if (chip.lastChild && chip.lastChild.nodeType === 3) chip.lastChild.nodeValue = next;
        }
        if (phone.getAttribute('data-current-voice') === oldName) selectVoice(next);
        toast('已改名为 ' + next);
      }
      input.addEventListener('keydown', function (event) {
        if (event.key === 'Enter') { event.preventDefault(); finish(true); }
        if (event.key === 'Escape') { event.preventDefault(); finish(false); }
      });
      input.addEventListener('blur', function () { finish(true); });
    }

    if (vmList) {
      vmList.addEventListener('click', function (event) {
        var row = event.target.closest('.vm-row');
        if (!row) return;
        if (event.target.closest('[data-delete]')) {
          event.stopPropagation();
          pendingDelete = row;
          var name = row.getAttribute('data-name');
          var title = $('[data-vm-confirm-title]');
          if (title) title.textContent = '删除“' + name + '”？';
          if (vmConfirm) {
            vmConfirm.classList.remove('show-error');
            vmConfirm.classList.add('show');
          }
          return;
        }
        if (event.target.closest('[data-rename]')) {
          event.stopPropagation();
          beginRename(row);
          return;
        }
        selectVoice(row.getAttribute('data-name'));
        toast('已切换到 ' + row.getAttribute('data-name'));
      });
    }

    $$('[data-vm-delete]').forEach(function (btn) {
      btn.addEventListener('click', function () {
        if (!pendingDelete) return;
        var name = pendingDelete.getAttribute('data-name');
        var chip = cloneChips().filter(function (c) { return c.getAttribute('data-voice') === name; })[0];
        if (chip) chip.remove();
        pendingDelete.remove();
        pendingDelete = null;
        if (vmConfirm) vmConfirm.classList.remove('show', 'show-error');
        updateCapacity();
        if (phone.getAttribute('data-current-voice') === name) {
          var next = cloneChips()[0];
          selectVoice(next ? next.getAttribute('data-voice') : '龙安欢');
        }
        toast('已删除 ' + name);
      });
    });

    $$('[data-vm-new]').forEach(function (btn) {
      btn.addEventListener('click', function () { openAudio(true, 'record'); });
    });
    $$('[data-vm-audition]').forEach(function (btn) {
      btn.addEventListener('click', function () {
        var current = $('[data-vm-current]');
        toast('试听 · ' + (current ? current.textContent : '当前音色'));
      });
    });

    function buildWave() {
      var wave = $('[data-audio-wave]');
      if (!wave || wave.children.length) return;
      var heights = [18, 31, 44, 27, 39, 52, 33, 22, 47, 36, 30, 55, 41, 26, 35, 49, 29, 43, 24, 38, 50, 32, 21, 45, 34, 28, 40, 25, 37, 48, 30, 20, 42, 33, 46, 27];
      heights.forEach(function (h, i) {
        var bar = document.createElement('i');
        bar.style.height = h + '%';
        bar.style.opacity = String(0.42 + (i % 5) * 0.09);
        wave.appendChild(bar);
      });
    }

    var createMode = 'record';
    var recordTimer = null;
    var recordDone = false;
    var recordElapsed = 0;
    var RECORD_MAX = 10;
    var createTabs = $$('[data-create-tab]');
    var recordPanel = $('[data-record-panel]');
    var uploadPanel = $('[data-upload-panel]');
    var recordPuck = $('[data-record-puck]');
    var recordTime = $('[data-record-time]');
    var recordStatus = $('[data-record-status]');
    var recordProgress = $('[data-record-progress]');
    var audioHint = $('[data-audio-hint]');
    var createAudioBtn = $('[data-create-audio]');

    function recordClock(seconds) {
      var s = Math.min(RECORD_MAX, Math.max(0, Math.floor(seconds)));
      return '00:' + String(s).padStart(2, '0');
    }
    function paintRecord() {
      if (recordTime) recordTime.textContent = recordClock(recordElapsed);
      if (recordProgress) recordProgress.style.width = (recordElapsed / RECORD_MAX * 100) + '%';
    }
    function setGenerateEnabled(on) {
      if (createAudioBtn) createAudioBtn.disabled = !on;
    }
    function resetRecording() {
      clearInterval(recordTimer);
      recordTimer = null;
      recordDone = false;
      recordElapsed = 0;
      if (recordPuck) {
        recordPuck.classList.remove('is-recording');
        recordPuck.setAttribute('aria-label', '开始录音');
      }
      paintRecord();
      if (recordStatus) recordStatus.textContent = '点击麦克风开始，安静环境下读 10 秒';
      setGenerateEnabled(false);
    }
    function stopRecording(auto) {
      if (!recordTimer) return;
      clearInterval(recordTimer);
      recordTimer = null;
      if (recordPuck) {
        recordPuck.classList.remove('is-recording');
        recordPuck.setAttribute('aria-label', '重新录音');
      }
      recordDone = recordElapsed >= 1;
      if (recordDone) {
        setGenerateEnabled(true);
        if (recordStatus) recordStatus.textContent = auto ? '录好了，可以生成音色' : '已录制 ' + Math.ceil(recordElapsed) + ' 秒，可以生成';
      } else {
        recordElapsed = 0;
        paintRecord();
        if (recordStatus) recordStatus.textContent = '录音太短，再试一次';
        setGenerateEnabled(false);
      }
    }
    function startRecording() {
      if (recordTimer) return;
      recordDone = false;
      recordElapsed = 0;
      if (recordPuck) {
        recordPuck.classList.add('is-recording');
        recordPuck.setAttribute('aria-label', '停止录音');
      }
      if (recordStatus) recordStatus.textContent = '正在录音… 请保持 10 秒';
      setGenerateEnabled(false);
      paintRecord();
      var started = performance.now();
      recordTimer = setInterval(function () {
        recordElapsed = Math.min(RECORD_MAX, (performance.now() - started) / 1000);
        paintRecord();
        if (recordElapsed >= RECORD_MAX) stopRecording(true);
      }, 100);
    }
    function setCreateMode(mode) {
      createMode = mode === 'upload' ? 'upload' : 'record';
      createTabs.forEach(function (tab) {
        var on = tab.getAttribute('data-create-tab') === createMode;
        tab.classList.toggle('is-on', on);
        tab.setAttribute('aria-selected', on ? 'true' : 'false');
      });
      if (recordPanel) recordPanel.hidden = createMode !== 'record';
      if (uploadPanel) uploadPanel.hidden = createMode !== 'upload';
      resetRecording();
      if (createMode === 'upload') {
        setGenerateEnabled(true);
        if (audioHint) audioHint.textContent = '保留 5–20 秒的人声；生成后会出现在“我的复刻”，也会同步到云端。';
      } else if (audioHint) {
        audioHint.textContent = '录完 10 秒后生成；建议用平常说话的音量。';
      }
    }
    function openAudio(fromManager, mode) {
      if (!audioModal) return;
      audioFromManager = !!fromManager;
      buildWave();
      setCreateMode(mode || 'record');
      openModal(audioModal);
      if (fromManager && voiceModal) voiceModal.classList.add('behind');
    }
    function closeAudio() {
      clearInterval(recordTimer);
      recordTimer = null;
      if (recordPuck) {
        recordPuck.classList.remove('is-recording');
        recordPuck.setAttribute('aria-label', '开始录音');
      }
      closeModal(audioModal);
      if (voiceModal) voiceModal.classList.remove('behind');
    }
    createTabs.forEach(function (tab) {
      tab.addEventListener('click', function () {
        setCreateMode(tab.getAttribute('data-create-tab'));
      });
    });
    if (recordPuck) recordPuck.addEventListener('click', function () {
      if (recordTimer) stopRecording(false);
      else startRecording();
    });
    $$('.lab-upload').forEach(function (btn) {
      btn.addEventListener('click', function (event) {
        event.preventDefault();
        event.stopImmediatePropagation();
        openAudio(false, 'upload');
      }, true);
    });
    $$('[data-close-audio]').forEach(function (btn) { btn.addEventListener('click', closeAudio); });
    $$('[data-cancel-audio]').forEach(function (btn) { btn.addEventListener('click', closeAudio); });
    if (audioModal) {
      audioModal.addEventListener('click', function (event) {
        if (event.target === audioModal) closeAudio();
      });
    }

    var audioFileInput = document.createElement('input');
    audioFileInput.type = 'file';
    audioFileInput.accept = 'audio/*';
    audioFileInput.className = 'lab-file';
    audioFileInput.setAttribute('aria-label', '选择要上传的音频');
    if (audioModal) audioModal.appendChild(audioFileInput);

    $$('.audio-file').forEach(function (row) {
      row.addEventListener('click', function () { audioFileInput.click(); });
    });
    audioFileInput.addEventListener('change', function () {
      var file = audioFileInput.files && audioFileInput.files[0];
      if (!file) return;
      var base = file.name.replace(/\.[^.]+$/, '').slice(0, 8) || '我的音色';
      var nameEl = $('[data-audio-file]');
      if (nameEl) nameEl.textContent = file.name;
      var copy = $('.audio-file-copy small');
      if (copy) copy.textContent = '已选择 · 读取时长中 · ' + Math.max(1, Math.round(file.size / 1024)) + ' KB';
      var nameInput = $('[data-audio-name-input]');
      if (nameInput) nameInput.value = base;
      toast('已选择 ' + file.name);
      audioFileInput.value = '';
    });

    $$('[data-audio-trim]').forEach(function (trim) {
      trim.addEventListener('input', function () {
        var end = $('[data-audio-trim-end]');
        if (end) end.textContent = '0:' + String(10 + Number(trim.value)).padStart(2, '0');
      });
    });

    $$('[data-create-audio]').forEach(function (btn) {
      btn.addEventListener('click', function () {
        if (createMode === 'record' && !recordDone) return;
        var input = $('[data-audio-name-input]');
        var name = input ? input.value.trim().slice(0, 8) : '';
        if (!name) name = '我的音色';
        addVoice(name);
        closeAudio();
        toast('复刻完成 · ' + name);
      });
    });

    function openReset() {
      if (!resetModal) return;
      openModal(resetModal);
      var input = $('[data-reset-input]');
      var confirm = $('[data-reset-confirm]');
      if (confirm) confirm.disabled = true;
      if (input) input.value = '';
    }
    function closeReset() { closeModal(resetModal); }
    $$('.reset-bar').forEach(function (btn) {
      btn.addEventListener('click', function (event) {
        event.preventDefault();
        openReset();
      });
    });
    $$('[data-close-reset]').forEach(function (btn) { btn.addEventListener('click', closeReset); });
    $$('[data-reset-cancel]').forEach(function (btn) { btn.addEventListener('click', closeReset); });
    if (resetModal) {
      resetModal.addEventListener('click', function (event) {
        if (event.target === resetModal) closeReset();
      });
    }
    var resetInput = $('[data-reset-input]');
    var resetConfirm = $('[data-reset-confirm]');
    if (resetInput && resetConfirm) {
      resetInput.addEventListener('input', function () {
        resetConfirm.disabled = resetInput.value.trim() !== '确定恢复出厂设置';
      });
      resetConfirm.addEventListener('click', function () {
        if (resetConfirm.disabled) return;
        closeReset();
        toast('已恢复出厂设置（原型）');
      });
    }

    // Dark mode is a preview switch; the chosen mode persists locally.
    function setMode(mode, silent) {
      phone.classList.toggle('dark', mode === 'dark');
      $$('[data-mode]').forEach(function (btn) {
        btn.setAttribute('aria-pressed', btn.getAttribute('data-mode') === mode ? 'true' : 'false');
      });
      if (!silent && window.PandaTheme) window.PandaTheme.set({ mode: mode });
    }
    $$('[data-mode]').forEach(function (btn) {
      btn.addEventListener('click', function () { setMode(btn.getAttribute('data-mode')); });
    });
    try {
      var savedMode = window.PandaTheme ? window.PandaTheme.get().mode : 'light';
      if (savedMode) setMode(savedMode, true);
    } catch (e) {}

    // Fixed voices drift left; pause while hovering, touching or scrolling manually.
    var rail = $('[data-fixed-rail]');
    var track = rail ? rail.querySelector('.fixed-track') : null;
    if (rail && track) {
      var originals = Array.prototype.slice.call(track.children);
      var originalCount = originals.length;
      originals.forEach(function (chip) {
        var clone = chip.cloneNode(true);
        clone.setAttribute('aria-hidden', 'true');
        clone.setAttribute('tabindex', '-1');
        track.appendChild(clone);
      });
      var loopPoint = 0;
      var loopDirty = true;
      function measureLoop() {
        if (!loopDirty && window.MutationObserver) return;
        loopDirty = false;
        var firstClone = track.children[originalCount];
        if (!firstClone) return;
        var delta = firstClone.offsetLeft - track.children[0].offsetLeft;
        if (delta > 0) loopPoint = delta;
      }
      var paused = false;
      var resumeTimer = null;
      var reduced = window.matchMedia && window.matchMedia('(prefers-reduced-motion: reduce)').matches;
      function pause() {
        paused = true;
        clearTimeout(resumeTimer);
      }
      function resumeSoon() {
        clearTimeout(resumeTimer);
        resumeTimer = setTimeout(function () { paused = false; }, 1800);
      }
      rail.addEventListener('pointerenter', pause);
      rail.addEventListener('pointerleave', resumeSoon);
      rail.addEventListener('pointerdown', pause);
      rail.addEventListener('pointerup', resumeSoon);
      rail.addEventListener('pointercancel', resumeSoon);
      rail.addEventListener('wheel', function () { pause(); resumeSoon(); }, { passive: true });
      rail.addEventListener('touchstart', pause, { passive: true });
      rail.addEventListener('touchend', resumeSoon);
      var lastStep = 0;
      var visibleSince = 0;
      var frame = 0;
      var intersecting = true;
      var driftActive = false;
      function railVisible() {
        if (document.hidden || !intersecting || reduced || !rail.getClientRects().length) return false;
        if (getComputedStyle(rail).visibility === 'hidden') return false;
        return rail.offsetParent !== null;
      }
      function drift(ts) {
        frame = 0;
        if (!driftActive) {
          visibleSince = 0;
          if (rail.scrollLeft) rail.scrollLeft = 0;
          return;
        }
        if (!visibleSince) {
          visibleSince = ts;
          rail.scrollLeft = 0;
          measureLoop();
        }
        if (!paused && ts - visibleSince > 2600) {
          measureLoop();
          if (loopPoint > 0 && ts - lastStep > 55) {
            lastStep = ts;
            var next = rail.scrollLeft + 1;
            if (next >= loopPoint) next -= loopPoint;
            rail.scrollLeft = next;
          }
        }
        frame = requestAnimationFrame(drift);
      }
      function syncDrift() {
        driftActive = railVisible();
        if (driftActive) {
          if (!frame) frame = requestAnimationFrame(drift);
        } else {
          if (frame) cancelAnimationFrame(frame);
          frame = 0;
          visibleSince = 0;
          if (rail.scrollLeft) rail.scrollLeft = 0;
        }
      }
      document.addEventListener('visibilitychange', syncDrift);
      // Opening the console changes visibility through the phone's class.
      if (window.MutationObserver) {
        new MutationObserver(syncDrift).observe(phone, { attributes: true, attributeFilter: ['class'] });
        new MutationObserver(function () {
          loopPoint = 0;
          loopDirty = true;
          syncDrift();
        }).observe(track, { childList: true, subtree: true, characterData: true });
      }
      if (window.ResizeObserver) {
        new ResizeObserver(function () {
          loopPoint = 0;
          loopDirty = true;
        }).observe(track);
      }
      if (!window.ResizeObserver) {
        window.addEventListener('resize', function () { loopPoint = 0; loopDirty = true; syncDrift(); });
      }
      if (window.IntersectionObserver) {
        new IntersectionObserver(function (entries) {
          intersecting = entries[0].isIntersecting;
          syncDrift();
        }).observe(rail);
      }
      syncDrift();
      track.addEventListener('click', function (event) {
        var chip = event.target.closest('.voice-chip');
        if (!chip) return;
        event.preventDefault();
        selectVoice(chip.getAttribute('data-voice'));
      });
    }

    // Escape closes the topmost in-page modal before the panel.
    document.addEventListener('keydown', function (event) {
      if (event.key !== 'Escape') return;
      var top = audioModal && audioModal.classList.contains('show') ? audioModal :
        (resetModal && resetModal.classList.contains('show') ? resetModal :
        (voiceModal && voiceModal.classList.contains('show') ? voiceModal : null));
      if (top) {
        event.preventDefault();
        event.stopImmediatePropagation();
        if (top === audioModal) closeAudio();
        else if (top === resetModal) closeReset();
        else closeVoiceManager();
      }
    }, true);

    updateCapacity();
  })();
