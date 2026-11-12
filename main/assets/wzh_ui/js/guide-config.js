/* 爬爬熊猫 · 引导文案（离线原型）
   文案只做“当前页怎么用”；有 action 的步骤，点高亮处或按“下一步”都会前进。 */
function pandaPick2g(evt) {
  var row = evt && evt.target && evt.target.closest ? evt.target.closest('.net-row') : null;
  return !!row && row.getAttribute('data-band') !== '5G';
}

window.PANDA_GUIDE_CONFIG = {
  login: {
    store: 'login',
    steps: [
      {
        target: '#loginPw',
        title: '先输入页面密码',
        text: '密码印在纸质说明书上。',
        gate: {
          selector: '#loginPw', on: 'input', minLength: 8,
          hint: '至少 8 位，输完才能继续',
          doneHint: '密码填好了，点下一步'
        }
      },
      {
        target: '#loginSubmit',
        title: '按一下，门就开了',
        text: '连错 5 次会锁 60 秒。',
        action: { selector: '#loginSubmit' },
        hint: '点一下“开门进入”'
      }
    ]
  },

  setupFirst: {
    store: 'setup',
    steps: [
      {
        target: '.setup-scene',
        title: '它就在窗边等你',
        text: '先连上家里的 Wi-Fi，再开始玩。'
      },
      {
        target: '#netList',
        place: 'above',
        title: '选一个 2.4GHz 网络',
        text: '列表可以上下滑，点任意一行。',
        gate: {
          selector: '#netList', on: 'click', test: pandaPick2g,
          hint: '先点一个 2.4GHz 网络',
          doneHint: '已选好，正在打开密码…', autoNext: 520
        }
      },
      {
        target: '#pwInput',
        title: '输入 Wi-Fi 密码',
        text: '密码区分大小写，填完点下一步。',
        prepare: function () {
          var layer = document.getElementById('pwLayer');
          if (layer && !layer.classList.contains('show')) {
            var row = document.querySelector('#netList .net-row');
            if (row) row.click();
          }
        },
        gate: {
          selector: '#pwInput', on: 'input', minLength: 8,
          hint: '至少 8 位，输完才能继续',
          doneHint: '密码填好了，点下一步'
        }
      },
      {
        target: '#pwConnect',
        title: '点连接，等它回家',
        text: '连上后会自动进入完成页。',
        action: { selector: '#pwConnect', delay: 260 },
        hint: '点一下“连接”'
      }
    ]
  },

  setupChange: {
    store: 'setupChange',
    steps: [
      {
        target: '#currentNet',
        title: '旧网络先留着',
        text: '换网失败会自动回到旧网络。'
      },
      {
        target: '#netList',
        place: 'above',
        title: '选一个新网络',
        text: '列表可以上下滑，点任意一行。',
        gate: {
          selector: '#netList', on: 'click', test: pandaPick2g,
          hint: '先点一个 2.4GHz 网络',
          doneHint: '已选好，正在打开密码…', autoNext: 520
        }
      },
      {
        target: '#pwInput',
        title: '输入新网络密码',
        text: '输完点下一步。',
        prepare: function () {
          var layer = document.getElementById('pwLayer');
          if (layer && !layer.classList.contains('show')) {
            var row = document.querySelector('#netList .net-row');
            if (row) row.click();
          }
        },
        gate: {
          selector: '#pwInput', on: 'input', minLength: 8,
          hint: '至少 8 位，输完才能继续',
          doneHint: '密码填好了，点下一步'
        }
      },
      {
        target: '#pwConnect',
        title: '点连接，完成换网',
        text: '失败会自动回到旧网络。',
        action: { selector: '#pwConnect', delay: 260 },
        hint: '点一下“连接”'
      }
    ]
  },

  setupOffline: {
    store: 'setupOffline',
    steps: [
      {
        target: '#setupBanner',
        title: '它现在离线',
        text: '旧网络还会继续自动重试。'
      },
      {
        target: '#netList',
        place: 'above',
        title: '换一个网络试试',
        text: '列表可以上下滑，点任意一行。',
        gate: {
          selector: '#netList', on: 'click', test: pandaPick2g,
          hint: '先点一个 2.4GHz 网络',
          doneHint: '已选好，正在打开密码…', autoNext: 520
        }
      },
      {
        target: '#pwInput',
        title: '输入 Wi-Fi 密码',
        text: '旧凭据会保留，直到新网络连上。',
        prepare: function () {
          var layer = document.getElementById('pwLayer');
          if (layer && !layer.classList.contains('show')) {
            var row = document.querySelector('#netList .net-row');
            if (row) row.click();
          }
        },
        gate: {
          selector: '#pwInput', on: 'input', minLength: 8,
          hint: '至少 8 位，输完才能继续',
          doneHint: '密码填好了，点下一步'
        }
      },
      {
        target: '#pwConnect',
        title: '点连接，重新上线',
        text: '连不上会回到原来的网络。',
        action: { selector: '#pwConnect', delay: 260 },
        hint: '点一下“连接”'
      }
    ]
  },

  doneSuccess: {
    store: 'done',
    steps: [
      {
        target: '#donePrimary',
        title: '已经连上了',
        text: '进去看电量、温度和音色。',
        action: { selector: '#donePrimary', seen: true },
        hint: '点“进入设备详情”'
      },
      {
        target: '#doneLater',
        title: '或者先留在这里',
        text: '之后重新打开热点也能进。',
        action: { selector: '#doneLater' },
        hint: '点“稍后再看”'
      }
    ]
  },

  doneFail: {
    store: 'done',
    steps: [
      {
        target: '#donePrimary',
        title: '回去改一下密码',
        text: '多半是 Wi-Fi 密码不对。',
        action: { selector: '#donePrimary' },
        hint: '点“返回修改”'
      },
      {
        target: '#doneLater',
        title: '也可以再试一次',
        text: '密码没改就原地重试。',
        action: { selector: '#doneLater' },
        hint: '点“再试一次”'
      }
    ]
  },

  detail: {
    store: 'detail',
    prepare: function (phone) {
      if (phone.__setManual) phone.__setManual(false);
      if (phone.__setExpanded) phone.__setExpanded(false);
      var more = phone.querySelector('.console-scroll') || phone.querySelector('.sheet-more');
      if (more) more.scrollTop = 0;
    },
    onDone: function () {
      var phone = document.querySelector('.phone');
      if (phone && phone.__setExpanded) phone.__setExpanded(false);
      if (phone && phone.__setManual) phone.__setManual(false);
    },
    steps: [
      {
        target: '.orb-wrap',
        title: '这是它的状态球',
        text: '待机、聆听、说话都会变。',
        state: 'collapsed'
      },
      {
        target: '.metrics',
        title: '一眼看状态',
        text: '电量、温度、网络，都是实时。',
        state: 'collapsed'
      },
      {
        target: '.sheet-main .primary',
        title: '想聊就点这里',
        text: '和双击电源键一样。',
        action: { selector: '.sheet-main .primary' },
        hint: '点一下“开始对话”',
        state: 'collapsed'
      },
      {
        target: '.voice-row',
        title: '声音都在这里',
        text: '点一下，打开完整控制台。',
        action: { selector: '.voice-row' },
        hint: '点一下当前音色',
        state: 'collapsed'
      },
      {
        target: '.con-head',
        title: '这是控制台',
        text: '设备、声音、网络和说明书都在这里。',
        state: 'expanded',
        prepare: function (phone) {
          var more = phone.querySelector('.console-scroll') || phone.querySelector('.sheet-more');
          if (more) more.scrollTop = 0;
        }
      },
      {
        target: '.network-tile',
        title: '网络和信号',
        text: '点“更改”可以换 Wi-Fi。',
        state: 'expanded'
      },
      {
        target: '.fixed-rail',
        title: '先挑一个固定音色',
        text: '左右滑动，点一下就能切换。',
        state: 'expanded'
      },
      {
        target: '.voice-lab',
        title: '声音实验室',
        text: '录 10 秒，或上传一段人声。',
        state: 'expanded'
      },
      {
        target: '.clone-strip',
        title: '复刻音色',
        text: '点管理，可以改名或删除。',
        state: 'expanded'
      },
      {
        target: '.con-volume',
        title: '音量也在这里',
        text: '拖动滑条，调到舒服的大小。',
        state: 'expanded'
      },
      {
        target: '.panel-theme',
        title: '整套主题都在这里',
        text: '布局、玻璃、颜色和明暗，全部在这一块；会同步到所有页面。',
        state: 'expanded'
      },
      {
        target: '.reset-bar',
        title: '要清空，来这里',
        text: '会先删掉云端复刻音色。',
        state: 'expanded'
      },
      {
        target: '.con-manual',
        title: '完整说明书',
        text: '按键、配网、声音和恢复出厂都写了。',
        action: { selector: '.con-manual' },
        hint: '点一下“说明书”',
        state: 'expanded'
      },
      {
        target: '.manual-close',
        title: '看完点 X',
        text: '回到控制台，接着玩。',
        action: { selector: '.manual-close' },
        hint: '点一下说明书右上角',
        state: 'expanded',
        prepare: function (phone) {
          if (phone.__setManual && !phone.classList.contains('manual-open')) phone.__setManual(true);
        }
      },
      {
        target: '.con-close',
        title: '最后收起控制台',
        text: '回到状态球，随时再点开。',
        action: { selector: '.con-close' },
        hint: '点一下右上角',
        state: 'expanded',
        prepare: function (phone) {
          if (phone.__setManual) phone.__setManual(false);
        }
      }
    ]
  },

  manual: {
    store: 'manual',
    steps: [
      {
        target: '.manual-nav',
        title: '先看目录',
        text: '左右滑动，挑一章。'
      },
      {
        target: '#m-faq .faq summary',
        title: '卡住就看这里',
        text: '点一下展开最短排查。',
        action: { selector: '#m-faq .faq summary' },
        hint: '点一下问题'
      },
      {
        target: '.appbar .back',
        title: '看完回详情',
        text: '左上角返回，说明书离线也能看。',
        action: { selector: '.appbar .back' },
        hint: '点一下返回'
      }
    ]
  }
};
