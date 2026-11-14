const config = require('../../services/config')
const MockClient = require('../../services/mock')
const products = require('../../services/products')
const store = require('../../services/store')
const fmt = require('../../utils/format')
const audio = require('../../services/audio')

const FIXED = ['龙安欢', '龙安灵心', '龙安风悦', '许南川', '于小云', '温怀清']
const THEME_LABEL = { milk: '奶糖', salt: '海盐', berry: '莓果', bamboo: '竹青' }
const MATERIAL_LABEL = { a: '通透', b: '奶润' }
const RECORD_FAIL = {
  busy: '设备正忙，请稍后再试',
  config: '语音服务未配置，请联系售后',
  prompt: '没有听清提示，请重试',
  memory: '设备内存不足，请重启后再试',
  short: '有效人声太短，请在提示后连续清晰朗读 10–15 秒',
  cloud: '云端繁忙或网络不稳定，请稍后重试',
  auth: '语音服务鉴权失败，请联系售后',
  limit: '今日复刻次数已用完，请明天再试',
  voice_full: '复刻音色已满，请先删除一个',
  uncertain: '生成结果不确定，请先检查音色列表',
  save: '音色保存失败，请重试',
  network: '网络不稳定，请检查玩具的 Wi-Fi 后重试'
}
const GUIDE_STEPS = [
  { target: '.orb-wrap', title: '这是它的状态球', text: '待机、聆听、说话都会变。', state: 'collapsed' },
  { target: '.metrics', title: '一眼看状态', text: '电量、温度、网络，都是实时。', state: 'collapsed' },
  { target: '.sheet-main .primary', title: '想聊就点这里', text: '和双击电源键一样。', state: 'collapsed' },
  { target: '.voice-row', title: '声音都在这里', text: '点一下，打开完整控制台。', state: 'collapsed' },
  { target: '.con-head', title: '这是控制台', text: '设备、声音、网络和说明书都在这里。', state: 'expanded' },
  { target: '.network-tile', title: '网络和信号', text: '点“更改”可以换 Wi-Fi。', state: 'expanded', panel: 'panel-device' },
  { target: '.fixed-rail', title: '先挑一个固定音色', text: '左右滑动，点一下就能切换。', state: 'expanded', panel: 'panel-sound' },
  { target: '.voice-lab', title: '声音实验室', text: '连续朗读一段话，或上传一段人声。', state: 'expanded', panel: 'panel-sound' },
  { target: '.clone-strip', title: '复刻音色', text: '点管理，可以改名或删除。', state: 'expanded', panel: 'panel-sound' },
  { target: '.volume-row.con-volume', title: '音量也在这里', text: '拖动滑条，调到舒服的大小。', state: 'expanded', panel: 'panel-sound' },
  { target: '.panel-theme', title: '整套主题都在这里', text: '布局、玻璃、颜色和明暗，全部在这一块。', state: 'expanded', panel: 'panel-theme' },
  { target: '.reset-bar', title: '要清空，来这里', text: '会先删掉云端复刻音色。', state: 'expanded', panel: 'panel-reset' },
  { target: '.con-manual', title: '完整说明书', text: '按键、配网、声音和恢复出厂都写了。', state: 'expanded' },
  { target: '.manual-close', title: '看完点 X', text: '回到控制台，接着玩。', state: 'expanded', manual: true },
  { target: '.con-close', title: '最后收起控制台', text: '回到状态球，随时再点开。', state: 'expanded', manual: false }
]
const MANUAL_NAV = [
  { id: 'm-start', label: '快速上手' },
  { id: 'm-parts', label: '认识它' },
  { id: 'm-power', label: '开关机' },
  { id: 'm-controls', label: '按键触摸' },
  { id: 'm-chat', label: '对话' },
  { id: 'm-network', label: '配网换网' },
  { id: 'm-app', label: '小程序' },
  { id: 'm-voice', label: '声音' },
  { id: 'm-motion', label: '动作' },
  { id: 'm-protect', label: '保护' },
  { id: 'm-faq', label: '常见问题' },
  { id: 'm-reset', label: '恢复出厂' },
  { id: 'm-spec', label: '规格' }
]

const MANUAL_COPY = {
  crawlPanda: {
    intro: '一只会抬头、会挥爪、会听你说话的桌面小伙伴。第一次使用，从这里开始。',
    partsIntro: '先认识它的六个部分。日常操作只会用到电源键、BOOT 键和头顶触摸区。',
    parts: [
      { n: 'A', t: '头部', d: '抬头、低头、左右看' },
      { n: 'B', t: '触摸区', d: '头顶，轻摸会回应' },
      { n: 'C', t: '电源键', d: '双击对话，长按开关机' },
      { n: 'D', t: 'BOOT 键', d: '单击切对话，双击停/恢复活动' },
      { n: 'E', t: '双前肢', d: '会挥爪、会撑起身体' },
      { n: 'F', t: '躯干与底座', d: '扬声器、麦克风、USB-C 充电口' }
    ],
    motionIntro: '它用头部和双前肢表达情绪。动作会根据声音大小、触摸和对话状态变化。',
    motionList: [
      { t: '待机', d: '轻轻呼吸，偶尔抬头看看四周。' },
      { t: '对话', d: '听到声音时头部抬起，前肢随语音包络摆动。' },
      { t: '亲昵', d: '轻摸头顶，抬头看你并轻轻挥爪。' },
      { t: '开心', d: '同时摸两个触摸区，动作幅度更大。' },
      { t: '说话', d: '回答时头部跟随语音节奏点头。' }
    ],
    inlineBody: '头部',
    inlineChat: '头部和双前肢',
    inlineWarn: '双前肢',
    specAction: '头部 1 路 + 双前肢 2 路舵机'
  },
  dinosaur: {
    intro: '一只会伸脖子、会甩尾巴、会听你说话的桌面小伙伴。第一次使用，从这里开始。',
    partsIntro: '先认识它的六个部分。日常操作只会用到电源键、BOOT 键和头顶触摸区。',
    parts: [
      { n: 'A', t: '头部', d: '抬头、低头、左右看' },
      { n: 'B', t: '触摸区', d: '头顶，轻摸会回应' },
      { n: 'C', t: '电源键', d: '双击对话，长按开关机' },
      { n: 'D', t: 'BOOT 键', d: '单击切对话，双击停/恢复活动' },
      { n: 'E', t: '颈部', d: '会前伸、后仰，凑近看你' },
      { n: 'F', t: '尾巴与躯干', d: '尾巴左右摆，躯干内有扬声器和充电口' }
    ],
    motionIntro: '它用颈部、头部和尾巴表达情绪。动作会根据声音大小、触摸和对话状态变化。',
    motionList: [
      { t: '待机', d: '慢慢呼吸，尾巴偶尔扫一下。' },
      { t: '对话', d: '听到声音时颈部前伸，尾巴跟着语音包络摆动。' },
      { t: '亲昵', d: '轻摸头顶，抬头看你并轻轻摆尾。' },
      { t: '开心', d: '同时摸两个触摸区，颈部与尾巴动作更大。' },
      { t: '说话', d: '回答时头部跟随语音节奏点头。' }
    ],
    inlineBody: '颈部、尾巴',
    inlineChat: '颈部和尾巴',
    inlineWarn: '颈部、尾巴',
    specAction: '颈部 2 路 + 头部 1 路 + 尾巴 2 路舵机'
  },
  redpanda: {
    intro: '一只会歪头、会摇大尾巴、会听你说话的桌面小伙伴。第一次使用，从这里开始。',
    partsIntro: '先认识它的六个部分。日常操作只会用到电源键、BOOT 键和头顶触摸区。',
    parts: [
      { n: 'A', t: '头部', d: '抬头、歪头、左右看' },
      { n: 'B', t: '触摸区', d: '头顶，轻摸会回应' },
      { n: 'C', t: '电源键', d: '双击对话，长按开关机' },
      { n: 'D', t: 'BOOT 键', d: '单击切对话，双击停/恢复活动' },
      { n: 'E', t: '大尾巴', d: '会左右摇，休息时盘在身后' },
      { n: 'F', t: '躯干与底座', d: '扬声器、麦克风、USB-C 充电口' }
    ],
    motionIntro: '它用头部和大尾巴表达情绪。动作会根据声音大小、触摸和对话状态变化。',
    motionList: [
      { t: '待机', d: '轻轻呼吸，大尾巴慢慢晃。' },
      { t: '对话', d: '听到声音时歪头，尾巴跟着语音包络摇动。' },
      { t: '亲昵', d: '轻摸头顶，歪头看你并快速摇尾。' },
      { t: '开心', d: '同时摸两个触摸区，头部与尾巴动作更大。' },
      { t: '说话', d: '回答时头部跟随语音节奏点头。' }
    ],
    inlineBody: '头部、大尾巴',
    inlineChat: '头部和大尾巴',
    inlineWarn: '大尾巴',
    specAction: '头部 1 路 + 尾巴 2 路舵机'
  }
}

function manualFor(product) {
  const base = MANUAL_COPY[product.id] || MANUAL_COPY.crawlPanda
  return Object.assign({}, base)
}

function faqFor(product) {
  const ap = product.ap + '-' + product.hot
  return [
    { q: '小程序搜不到设备怎么办？', a: '确认玩具已开机、手机蓝牙已开启，并且距离在 1 米内。设备在列表里显示为 ' + ap + '；若仍搜不到，长按电源键关机再开机，然后在连接页点“重新扫描”。' },
    { q: '连接后很快断开？', a: '先确认手机没有同时连接其他蓝牙音频设备，并把手机靠近玩具，避免隔着墙或金属桌面。iOS 需要允许微信使用蓝牙权限。' },
    { q: '连不上家里的 Wi-Fi？', a: '设备只支持 2.4GHz，5GHz 网络不会出现在列表里。密码至少 8 位；确认路由器已开启 DHCP，且没有开启 MAC 地址过滤。' },
    { q: '设备离线还能用吗？', a: '可以。离线时仍能用蓝牙连接小程序查看状态、更改网络和恢复出厂；但云端对话和复刻音色需要设备联网。' },
    { q: '复刻音色失败？', a: '确认设备在线、环境安静、一个人连续说话 5–20 秒。提示“复刻音色已满”时，先在音色管理里删除一个再试。' },
    { q: '触摸没有反应？', a: '对话进行中触摸只驱动动作，不额外播放声音。若完全没反应，确认没有长时间按住多个触摸区，或重启设备再试。' },
    { q: '设备发热正常吗？', a: '舵机连续动作或边充边用会发热，属于正常现象。温度过高时设备会限制动作；请放在通风处，避免阳光直射。' },
    { q: '双击电源键没反应？', a: '确认电量充足、设备已开机。正在说话时双击会结束本轮对话，等提示音结束后再双击开始。' }
  ]
}

const DEFAULT_STATUS = {
  online: true,
  state: 'idle',
  battery: 86,
  charging: true,
  temp: 41,
  tempState: '正常',
  runtimeMinutes: 138,
  runtime: '2h18',
  ssid: 'Home',
  band: '2.4G',
  signal: '强',
  rssi: -48,
  latency: 18,
  volume: 60,
  currentVoice: '龙安欢',
  version: 'v1.0.8',
  voiceCount: 6,
  voiceCapacity: 10
}

function platform() {
  try {
    if (wx.getDeviceInfo) return wx.getDeviceInfo().platform || 'unknown'
    return wx.getSystemInfoSync().platform || 'unknown'
  } catch (e) { return 'unknown' }
}

function isDevtools() {
  const p = platform()
  return p === 'devtools' || p === 'windows' || p === 'mac'
}

function windowInfo() {
  try {
    if (wx.getWindowInfo) return wx.getWindowInfo()
    return wx.getSystemInfoSync()
  } catch (e) { return { statusBarHeight: 20 } }
}

function pad(n) { return n < 10 ? '0' + n : '' + n }

function stateCopy(online, state) {
  if (!online) return { sheet: '离线', cta: '设备离线', disabled: true }
  switch (state) {
    case 'listening': return { sheet: '聆听中', cta: '结束对话', disabled: false }
    case 'speaking': return { sheet: '说话中', cta: '结束对话', disabled: false }
    case 'connecting': return { sheet: '连接中', cta: '连接中…', disabled: true }
    case 'offline': return { sheet: '离线', cta: '设备离线', disabled: true }
    default: return { sheet: '待机', cta: '开始对话', disabled: false }
  }
}

Page({
  data: {
    statusBarHeight: 20,
    menuBottom: 86,
    product: products.get('crawlPanda'),
    toys: ['crawlPanda', 'dinosaur', 'redpanda'].map(function (id) {
      const p = products.get(id)
      return { id: p.id, name: p.name, english: p.english, short: p.short, icon: p.icon }
    }),
    toyPickerOpen: false,
    theme: 'milk',
    material: 'b',
    layout: 'c',
    mode: 'light',
    themeLabel: '奶糖',
    materialLabel: '奶润',
    status: Object.assign({}, DEFAULT_STATUS),
    demoMode: false,
    deviceState: 'idle',
    sheetState: '待机',
    ctaLabel: '开始对话',
    ctaDisabled: false,
    voiceInitial: '龙',
    expanded: false,
    manualOpen: false,
    assistantX: 0,
    assistantY: 0,
    assistantDragging: false,
    assistDim: false,
    scrollIntoView: '',
    fixedVoices: FIXED.slice(),
    clones: [],
    cloneCount: 6,
    voiceCapacity: 10,
    signalLevels: [1, 2, 3, 4],
    waveLevels: [1, 2, 3, 4, 5],
    railPaused: false,
    railScroll: 0,
    auditioning: false,
    voiceModal: false,
    vmCurrent: '龙安欢',
    vmCurrentInitial: '龙',
    vmConfirm: false,
    vmPendingId: '',
    vmPendingName: '',
    vmError: '',
    editingId: '',
    editingValue: '',
    audioModal: false,
    audioMode: 'record',
    audioRecording: false,
    audioRecordPending: false,
    audioCreating: false,
    audioElapsed: 0,
    audioTime: '00:00',
    audioProgress: 0,
    audioDone: false,
    audioName: '我的音色',
    audioFileName: '',
    audioFileInfo: '',
    audioFile: '',
    audioError: '',
    audioHint: '提示后连续清晰朗读 10–15 秒，玩具最多录 20 秒。',
    audioRecordStatus: '点击麦克风开始，安静环境下连续朗读',
    audioTrim: 0,
    audioWave: [16, 28, 42, 30, 54, 38, 22, 48, 62, 34, 26, 44, 58, 36, 20, 52, 66, 40, 24, 46, 60, 32, 18, 50, 64, 38, 22, 42, 56, 30, 18, 36, 50, 28, 16, 44, 58, 34, 20, 48],
    resetModal: false,
    resetInput: '',
    resetReady: false,
    resetError: '',
    resetStage: '',
    resetting: false,
    wifiModal: false,
    wifiScanning: false,
    wifiList: [],
    wifiSelected: '',
    wifiNeedPassword: false,
    wifiPassword: '',
    wifiError: '',
    wifiConnecting: false,
    wifiCurrent: 'Home',
    wifiCurrentSub: '2.4G · 已连接',
    wifiFocus: false,
    toastShow: false,
    toastText: '',
    manual: {},
    manualNav: MANUAL_NAV,
    manualFaq: [],
    faqOpen: {},
    manualAnchor: '',
    guideOpen: false,
    guideStep: 0,
    guideDots: GUIDE_STEPS.map(function (_, i) { return i }),
    guideCurrent: { title: '', text: '' },
    guideSpot: { left: 0, top: 0, width: 0, height: 0, radius: 18 },
    guideBubble: { place: 'below', style: 'opacity:0' }
  },

  onLoad(options) {
    const info = windowInfo()
    const product = products.get((options && options.toy) || store.getToy())
    const appearance = store.getTheme(product.theme)
    const app = getApp()
    app.globalData.theme = appearance
    this.client = app.globalData.ble || null
    this.mock = !!app.globalData.mock
    this._recordTimer = null
    this._volumeTimer = null
    this._toastTimer = null
    this._auditionTimer = null
    this._wifiTimer = null
    this._resetDoneResolve = null
    this._renaming = false
    this._leaving = false

    let menuBottom = (info.statusBarHeight || 20) + 46
    try {
      const menu = wx.getMenuButtonBoundingClientRect()
      if (menu && menu.bottom) menuBottom = menu.bottom
    } catch (e) {}

    const winW = Number(info.windowWidth) || 390
    const winH = Number(info.windowHeight) || 844
    const faSize = winW * 100 / 750
    const faMargin = winW * 26.92 / 750
    const assistantX = Math.max(8, winW - faSize - faMargin)
    const assistantY = Math.max((info.statusBarHeight || 20) + 8, winH - faSize - faMargin)

    this.setData({
      statusBarHeight: info.statusBarHeight || 20,
      menuBottom: menuBottom,
      assistantX: assistantX,
      assistantY: assistantY,
      product: product,
      manual: manualFor(product),
      manualFaq: faqFor(product),
      theme: appearance.theme,
      material: appearance.material,
      layout: appearance.layout,
      mode: appearance.mode,
      themeLabel: THEME_LABEL[appearance.theme] || '奶糖',
      materialLabel: MATERIAL_LABEL[appearance.material] || '奶润'
    })

    if (!this.client) {
      if (isDevtools()) {
        this.client = new MockClient()
        this.mock = true
        const demoId = product.id === 'dinosaur' ? 'mock-dino' : (product.id === 'redpanda' ? 'mock-red' : 'mock-crawl')
        this.client.connect(demoId).then(res => {
          app.setBle(this.client, true)
          this.syncDevice(res.device)
          this.applyStatus(res.status)
        }).catch(() => {})
      } else {
        wx.showToast({ title: '请先连接设备', icon: 'none' })
        setTimeout(() => wx.redirectTo({ url: '/pages/connect/connect' }), 800)
        return
      }
    }

    this.setData({ demoMode: this.mock })
    this.unsub = this.client.onEvent((type, data) => this.onBleEvent(type, data))
    this.syncDevice(this.client.device)
    const last = app.globalData.lastStatus || this.client.status || null
    this.applyStatus(last)
    this.refreshStatus()
    this.refreshVoices()
    this.startStatusTimer()
  },

  onShow() {
    if (this.client) {
      this.refreshStatus()
      this.startStatusTimer()
    }
  },

  onHide() {
    this.stopStatusTimer()
  },

  startStatusTimer() {
    this.stopStatusTimer()
    this._statusTimer = setInterval(() => this.pollStatus(), 3000)
  },

  stopStatusTimer() {
    if (this._statusTimer) clearInterval(this._statusTimer)
    this._statusTimer = null
  },

  pollStatus() {
    if (!this.client) return
    if (this.data.audioRecordPending && this._recordPolling) {
      this.pollDeviceRecord()
      return
    }
    if (this.data.resetting || this.data.audioRecording || this.data.audioRecordPending || this.data.audioCreating || this.data.wifiConnecting) return
    this.refreshStatus()
  },

  onUnload() {
    this.stopStatusTimer()
    this.stopRailDrift()
    if (this._recordTimer) clearInterval(this._recordTimer)
    if (this._volumeTimer) clearTimeout(this._volumeTimer)
    if (this._toastTimer) clearTimeout(this._toastTimer)
    if (this._auditionTimer) clearTimeout(this._auditionTimer)
    if (this._wifiTimer) clearTimeout(this._wifiTimer)
    if (this._railTimer) clearTimeout(this._railTimer)
    if (this.unsub) this.unsub()
    if (this.client && !this._leaving) {
      this._leaving = true
      try { this.client.close() } catch (e) {}
      const app = getApp()
      if (app && app.clearBle) app.clearBle()
    }
  },

  normalizeStatus(raw) {
    const base = this.data.status || DEFAULT_STATUS
    const s = Object.assign({}, base, raw || {})
    const runtimeMinutes = Number(s.runtimeMinutes || 0)
    const volume = Number(s.volume)
    return {
      online: s.online !== false,
      state: s.state || 'idle',
      battery: fmt.clamp(Number(s.battery != null ? s.battery : 0), 0, 100),
      charging: !!s.charging,
      // 真机上报的是浮点（51.79999…），界面只显示取整值；分级文案也在这里统一算。
      temp: fmt.formatTemp(s.temp != null ? s.temp : s.temperature),
      tempClass: fmt.tempLevel(s.temp != null ? s.temp : s.temperature),
      tempState: s.temp != null ? fmt.tempStateText(s.temp) : (s.tempState || '正常'),
      runtimeMinutes: runtimeMinutes,
      runtime: (raw && raw.runtime) || fmt.formatRuntime(runtimeMinutes),
      ssid: s.ssid || '',
      band: s.band || '2.4G',
      signal: s.signal || '强',
      signalBars: fmt.signalBars(Number(s.rssi != null ? s.rssi : -60)),
      rssi: Number(s.rssi != null ? s.rssi : -60),
      latency: Number(s.latency || 0),
      latencyText: fmt.formatLatency(s.latency),
      volume: fmt.clamp(Number.isFinite(volume) ? volume : 0, 0, 100),
      currentVoice: s.currentVoice || '龙安欢',
      version: s.version || 'v1.0.8',
      voiceCount: Number(s.voiceCount != null ? s.voiceCount : this.data.cloneCount),
      voiceCapacity: Number(s.voiceCapacity || this.data.voiceCapacity)
    }
  },

  applyStatus(raw) {
    const s = this.normalizeStatus(raw)
    const copy = stateCopy(s.online, s.state)
    const app = getApp()
    if (app && app.globalData) app.globalData.lastStatus = s
    this.setData({
      status: s,
      deviceState: s.online ? s.state : 'offline',
      sheetState: copy.sheet,
      ctaLabel: copy.cta,
      ctaDisabled: copy.disabled,
      voiceInitial: fmt.initial(s.currentVoice),
      vmCurrent: s.currentVoice,
      vmCurrentInitial: fmt.initial(s.currentVoice),
      wifiCurrent: s.ssid || '',
      wifiCurrentSub: s.online ? (s.band + ' · 已连接') : '未连接',
      // 状态球跟着设备的真实状态与音量走，而不是固定节奏地“演”。
      orbVars: this.orbVars(s)
    })
  },

  // --orb-e: 0..1 能量（决定光晕亮度）；--orb-k: 动画时长倍率（越小越快）。
  orbVars(s) {
    const volume = fmt.clamp(Number(s.volume) || 0, 0, 100) / 100
    let energy = 0.3
    if (!s.online) energy = 0.16
    else if (s.state === 'speaking') energy = 0.55 + volume * 0.45
    else if (s.state === 'listening') energy = 0.5
    else if (s.state === 'connecting') energy = 0.4
    const speed = (1.15 - energy * 0.6).toFixed(2)
    return '--orb-e: ' + energy.toFixed(2) + '; --orb-k: ' + speed
  },

  syncDevice(device) {
    const dev = device || (this.client && this.client.device)
    if (!dev) return
    const product = products.detectProduct(dev)
    if (product) this.setData({ product: product })
    const fixed = this.voiceNames(dev.fixedVoices)
    if (fixed.length) this.setData({ fixedVoices: fixed })
    if (dev.clones) this.applyClones(dev.clones, dev.voiceCapacity)
  },

  voiceNames(list) {
    return (list || []).map(item => (typeof item === 'string' ? item : (item && item.name) || '')).filter(Boolean)
  },

  applyClones(list, capacity) {
    const known = {}
    ;(this.data.clones || []).forEach(c => { if (c && c.id) known[c.id] = c })
    const clones = (list || []).map(item => Object.assign({}, item, {
      initial: fmt.initial(item.name),
      // 固件 voice_list 只返回 id/name；保留本机已记录的创建日期，避免列表出现空日期。
      date: item.date || (known[item.id] && known[item.id].date) || ''
    }))
    this.setData({
      clones: clones,
      cloneCount: clones.length,
      voiceCapacity: capacity || this.data.voiceCapacity
    })
  },

  applyVoiceList(payload) {
    if (!payload) return
    let clones = payload.clones
    let capacity = payload.capacity || payload.voiceCapacity
    if (!clones && Array.isArray(payload.voices)) {
      clones = payload.voices.filter(v => v && !v.builtin)
      capacity = capacity || this.data.voiceCapacity
      const fixed = payload.voices.filter(v => v && v.builtin).map(v => v.name).filter(Boolean)
      if (fixed.length) this.setData({ fixedVoices: fixed })
    }
    if (clones) this.applyClones(clones, capacity)
    if (payload.currentVoice) {
      this.setData({
        'status.currentVoice': payload.currentVoice,
        voiceInitial: fmt.initial(payload.currentVoice),
        vmCurrent: payload.currentVoice,
        vmCurrentInitial: fmt.initial(payload.currentVoice)
      })
    }
  },

  async refreshStatus() {
    if (!this.client || this._statusBusy) return
    this._statusBusy = true
    try {
      const status = await this.client.request('get_status')
      if (status) {
        // 固件的 latency 字段目前恒为 0，这里用一次 ping 的往返时间做真实值。
        const rtt = await this.measureLatency()
        if (rtt > 0) status.latency = rtt
        this.applyStatus(status)
      }
    } catch (e) {
    } finally {
      this._statusBusy = false
    }
  },

  async measureLatency() {
    if (!this.client) return 0
    const start = Date.now()
    try {
      await this.client.request('ping', {}, 1200)
      return Date.now() - start
    } catch (e) {
      return 0
    }
  },

  async refreshVoices() {
    if (!this.client) return
    try {
      const voices = await this.client.request('voice_list')
      this.applyVoiceList(voices)
    } catch (e) {}
  },

  onBleEvent(type, data) {
    if (type === 'status' && data) {
      this.applyStatus(data)
      if (data.clones) this.applyClones(data.clones, data.voiceCapacity)
    } else if (type === 'wifi_state' && data) {
      if (data.state === 'connecting') {
        this.setData({ wifiConnecting: true, wifiError: '' })
      } else if (data.state === 'connected') {
        if (this._wifiTimer) clearTimeout(this._wifiTimer)
        this.setData({
          wifiConnecting: false,
          wifiModal: false,
          wifiSelected: '',
          wifiPassword: '',
          wifiCurrent: data.ssid || this.data.wifiCurrent,
          wifiCurrentSub: '2.4G · 已连接'
        })
        this.applyStatus(Object.assign({}, this.data.status, { online: true, ssid: data.ssid || this.data.status.ssid, band: '2.4G' }))
        this.toast('网络已连接 · ' + (data.ssid || ''))
      } else if (data.state === 'failed') {
        if (this._wifiTimer) clearTimeout(this._wifiTimer)
        this.setData({ wifiConnecting: false, wifiError: data.message || '连接失败，请检查密码' })
      }
    } else if (type === 'voice_record' && data) {
      if (!this.data.audioRecordPending) return
      const status = String(data.status || '')
      const rank = { starting: 1, prompt: 2, recording: 3, processing: 4, success: 5, failed: 5, cancelled: 5 }
      if (!rank[status] || rank[status] < (this._recordPhaseRank || 0)) return
      this._recordPhaseRank = rank[status]
      if (status === 'starting' || status === 'prompt') {
        this.setData({ audioRecordStatus: '正在准备，请听玩具提示…', audioError: '' })
      } else if (status === 'recording') {
        if (!this.data.audioRecording) {
          this.setData({ audioRecording: true, audioRecordStatus: '玩具正在录音，请连续清晰朗读 10–15 秒', audioError: '' })
          this.startRecordClock()
        }
      } else if (status === 'processing') {
        const reason = String(data.reason || '')
        this.setData({ audioRecording: false, audioRecordStatus: '录音完成，正在处理…', audioProgress: 100,
          audioError: RECORD_FAIL[reason] || '' })
        this.stopRecordClock()
      } else if (status === 'success') {
        this.finishDeviceRecord(data.name)
      } else if (status === 'failed' || status === 'cancelled') {
        const reason = String(data.reason || '')
        this.failDeviceRecord(status === 'cancelled'
          ? '录音已取消，请重新开始'
          : (RECORD_FAIL[reason] || '录音失败，请检查网络后重试'))
      }
    } else if (type === 'reset' && data) {
      const phase = String(data.phase || data.stage || '')
      if (phase === 'checking' || phase === 'start') {
        this.setData({ resetStage: '正在校验设备状态…' })
      } else if (phase === 'deleting_cloud') {
        this.setData({ resetStage: '正在删除云端复刻音色…' })
      } else if (phase === 'clearing_local') {
        this.setData({ resetStage: '正在清除本机复刻音色…' })
      } else if (phase === 'clearing_wifi') {
        this.setData({ resetStage: '正在清除网络配置…' })
      } else if (phase === 'rebooting' || phase === 'done') {
        if (this._resetDoneResolve) this._resetDoneResolve({ ok: true })
        this._resetDoneResolve = null
      } else if (phase === 'failed') {
        const message = (data.error && data.error.message) || data.message || '恢复失败，请重试'
        if (this._resetDoneResolve) this._resetDoneResolve({ ok: false, message: message })
        this._resetDoneResolve = null
      }
    } else if (type === 'disconnected') {
      if (this.data.resetting && this._resetDoneResolve) {
        this._resetDoneResolve({ ok: true })
        this._resetDoneResolve = null
        // 恢复出厂重启会主动断开蓝牙，这是预期行为，不要当成异常掉线。
        return
      }
      if (this._leaving) return
      this._leaving = true
      this.applyStatus(Object.assign({}, this.data.status, { online: false, state: 'offline' }))
      this.toast('设备已断开')
      const app = getApp()
      if (app && app.clearBle) app.clearBle()
      setTimeout(() => wx.redirectTo({ url: '/pages/connect/connect' }), 1200)
    }
  },

  onAssistantStart(e) {
    const t = e.touches && e.touches[0]
    if (!t) return
    this._assistantStart = {
      x: t.clientX,
      y: t.clientY,
      left: this.data.assistantX,
      top: this.data.assistantY
    }
    this._assistantMoved = false
    this.setData({ assistantDragging: true })
  },

  onAssistantMove(e) {
    if (!this._assistantStart) return
    const t = e.touches && e.touches[0]
    if (!t) return
    const dx = t.clientX - this._assistantStart.x
    const dy = t.clientY - this._assistantStart.y
    if (Math.abs(dx) + Math.abs(dy) > 4) this._assistantMoved = true
    const info = windowInfo()
    let menuBottom = (info.statusBarHeight || 20) + 46
    try {
      const menu = wx.getMenuButtonBoundingClientRect()
      if (menu && menu.bottom) menuBottom = menu.bottom
    } catch (e) {}

    const winW = Number(info.windowWidth) || 390
    const winH = Number(info.windowHeight) || 844
    const size = winW * 100 / 750
    const minX = 8
    const minY = (this.data.statusBarHeight || 20) + 8
    const maxX = Math.max(minX, winW - size - 8)
    const maxY = Math.max(minY, winH - size - 8)
    this.setData({
      assistantX: Math.min(maxX, Math.max(minX, this._assistantStart.left + dx)),
      assistantY: Math.min(maxY, Math.max(minY, this._assistantStart.top + dy)),
      assistantDragging: true
    })
  },

  onAssistantEnd() {
    const moved = this._assistantMoved
    this._assistantStart = null
    this._assistantMoved = false
    this.setData({ assistantDragging: false })
    if (!moved) this.onPrimary()
  },

  onAssistantCancel() {
    this._assistantStart = null
    this._assistantMoved = false
    this.setData({ assistantDragging: false })
  },

  openToyPicker() { this.setData({ toyPickerOpen: true }) },

  closeToyPicker() { this.setData({ toyPickerOpen: false }) },

  chooseToy(e) {
    const id = e.currentTarget.dataset.id
    if (!id) return
    store.setToy(id)
    this.setData({ toyPickerOpen: false })
    this._leaving = true
    if (this.client) { try { this.client.close() } catch (err) {} }
    const app = getApp()
    if (app && app.clearBle) app.clearBle()
    wx.reLaunch({ url: '/pages/connect/connect?toy=' + id })
  },

  toggleConsole() {
    if (this.data.manualOpen) return
    const expanded = !this.data.expanded
    this.setData({ expanded: expanded, railPaused: false, scrollIntoView: '', assistDim: false }, () => {
      this.syncRailDrift()
      if (expanded) this.measureConsoleView()
    })
  },

  closeConsole() {
    this.setData({ expanded: false, assistDim: false }, () => this.syncRailDrift())
  },

  // 滚到底部时把悬浮小助手淡出，避免它压住“恢复出厂”条。
  measureConsoleView() {
    setTimeout(() => {
      wx.createSelectorQuery().in(this)
        .select('.console-scroll')
        .boundingClientRect(rect => { if (rect && rect.height) this._consoleViewH = rect.height })
        .exec()
    }, 620)
  },

  onConsoleScroll(e) {
    const d = e && e.detail ? e.detail : null
    const viewH = this._consoleViewH || 0
    if (!d || !viewH) return
    // 只有内容真的能滚动、且滚到最底部附近时才淡出小助手。
    const scrollable = d.scrollHeight > viewH + 8
    const near = scrollable && (d.scrollHeight - d.scrollTop - viewH) < 48
    if (near !== !!this.data.assistDim) this.setData({ assistDim: near })
  },

  openSoundPanel() {
    this.setData({ expanded: true, scrollIntoView: 'panel-sound', assistDim: false })
    this.measureConsoleView()
    setTimeout(() => this.setData({ scrollIntoView: '' }), 400)
  },


  openGuide() {
    this._guidePending = null
    this.setData({
      guideOpen: true,
      guideStep: 0,
      guideCurrent: { title: GUIDE_STEPS[0].title, text: GUIDE_STEPS[0].text },
      guideSpot: { left: 0, top: 0, width: 0, height: 0, radius: 18 },
      guideBubble: { place: 'below', style: 'opacity:0' },
      guideHide: false,
      manualOpen: false,
      expanded: false,
      scrollIntoView: ''
    }, () => { this.syncRailDrift(); setTimeout(() => this.showGuideStep(0), 120) })
  },

  showGuideStep(index) {
    const step = GUIDE_STEPS[index]
    if (!step) return this.closeGuide()
    // 等上一步测量完成再切换，避免卡片/聚光灯停在旧位置造成残影。
    if (this._guidePending !== undefined && this._guidePending !== null) return
    this._guidePending = index
    const patch = { scrollIntoView: step.panel || '' }
    if (step.state === 'expanded') patch.expanded = true
    if (step.state === 'collapsed') patch.expanded = false
    if (step.manual === true) patch.manualOpen = true
    if (step.manual === false) patch.manualOpen = false
    // 展开/收起控制台或开关说明书时，先收起气泡与高亮（整屏均匀变暗），布局稳定后再一次性出现。
    if (patch.expanded !== undefined && patch.expanded !== this.data.expanded) patch.guideHide = true
    if (patch.manualOpen !== undefined && patch.manualOpen !== this.data.manualOpen) patch.guideHide = true
    if (patch.guideHide) patch.guideSpot = { left: -40, top: -40, width: 0, height: 0, radius: 0 }
    this.setData(patch, () => {
      this.syncRailDrift()
      setTimeout(() => this.measureGuide(index), step.state === 'expanded' ? 560 : 380)
    })
  },

  measureGuide(index) {
    const step = GUIDE_STEPS[index]
    if (!this.data.guideOpen || !step) { this._guidePending = null; return }
    const query = wx.createSelectorQuery()
    query.select('.phone').boundingClientRect()
    query.select(step.target).boundingClientRect()
    query.exec((res) => {
      if (!this.data.guideOpen) { this._guidePending = null; return }
      const phone = res && res[0]
      const rect = res && res[1]
      const vw = (phone && phone.width) || 390
      const vh = (phone && phone.height) || 700
      const spot = rect ? {
        left: Math.max(0, rect.left - 6),
        top: Math.max(0, rect.top - 6),
        width: Math.min(vw, rect.width + 12),
        height: Math.min(vh, rect.height + 12),
        radius: step.round ? 999 : 18
      } : { left: vw / 2 - 60, top: vh / 2 - 40, width: 120, height: 80, radius: 18 }
      const bw = Math.min(300, vw - 28)
      const below = spot.top + spot.height / 2 < vh * 0.56
      const left = Math.max(14, Math.min(spot.left + spot.width / 2 - bw / 2, vw - bw - 14))
      const style = below
        ? 'left:' + left + 'px;top:' + (spot.top + spot.height + 14) + 'px;width:' + bw + 'px'
        : 'left:' + left + 'px;bottom:' + Math.max(14, vh - spot.top + 14) + 'px;width:' + bw + 'px'
      // 聚光灯、气泡位置、标题正文一次性换到新目标（没有任何过渡），不留旧框。
      this._guidePending = null
      this.setData({
        guideStep: index,
        guideCurrent: { title: step.title, text: step.text },
        guideSpot: { left: spot.left, top: spot.top, width: spot.width, height: spot.height, radius: spot.radius },
        guideBubble: { place: below ? 'below' : 'above', style: style },
        guideHide: false
      })
    })
  },

  nextGuide() {
    if (this._guidePending !== undefined && this._guidePending !== null) return
    if (this.data.guideStep + 1 >= GUIDE_STEPS.length) return this.closeGuide()
    this.showGuideStep(this.data.guideStep + 1)
  },

  closeGuide() {
    this._guidePending = null
    this.setData({ guideOpen: false, expanded: false, manualOpen: false, scrollIntoView: '' }, () => this.syncRailDrift())
  },

  noop() {},

  openManual() {
    this.setData({ manualOpen: true })
  },

  closeManual() {
    this.setData({ manualOpen: false })
  },

  goManual(e) {
    const id = (e && e.currentTarget && e.currentTarget.dataset && e.currentTarget.dataset.id) || ''
    if (!id) return
    this.setData({ manualAnchor: id })
    if (this._manualTimer) clearTimeout(this._manualTimer)
    this._manualTimer = setTimeout(() => this.setData({ manualAnchor: '' }), 420)
  },

  toggleFaq(e) {
    const index = Number(e && e.currentTarget && e.currentTarget.dataset && e.currentTarget.dataset.index)
    if (isNaN(index)) return
    const faqOpen = Object.assign({}, this.data.faqOpen)
    faqOpen[index] = !faqOpen[index]
    this.setData({ faqOpen: faqOpen })
  },

  onScrim() {
    if (this.data.manualOpen) return this.closeManual()
    if (this.data.expanded) return this.closeConsole()
  },

  onPrimary() {
    if (this.data.ctaDisabled) return
    const idle = this.data.status.state === 'idle' || this.data.status.state === 'offline'
    const next = idle ? 'speaking' : 'idle'
    const prev = this.data.status.state
    this.applyStatus(Object.assign({}, this.data.status, { state: next }))
    if (!this.client) return
    this.client.request(idle ? 'chat_start' : 'chat_stop', {}).then(() => {
      setTimeout(() => this.refreshStatus(), 700)
    }).catch(() => {
      this.applyStatus(Object.assign({}, this.data.status, { state: prev }))
      this.toast('设备暂不支持语音会话控制')
    })
  },

  onTryVoice() {
    if (this.data.auditioning) return
    const name = this.data.status.currentVoice
    this.setData({ auditioning: true })
    if (this._auditionTimer) clearTimeout(this._auditionTimer)
    if (!this.client) {
      this.setData({ auditioning: false })
      return this.toast('设备未连接')
    }
    this.client.request('audition', { name: name }).then(() => {
      this.toast('正在试听 · ' + name)
      this._auditionTimer = setTimeout(() => this.setData({ auditioning: false }), 1600)
    }).catch(err => {
      this.setData({ auditioning: false })
      this.toast(err.message || '试听失败')
    })
  },

  onSelectVoice(e) {
    const name = e.currentTarget.dataset.voice
    this.selectVoice(name)
  },

  selectVoice(name) {
    if (!name || !this.client) return
    this.client.request('set_voice', { name: name }).then(result => {
      const selected = (result && result.name) || name
      this.setData({
        'status.currentVoice': selected,
        voiceInitial: fmt.initial(selected),
        vmCurrent: selected,
        vmCurrentInitial: fmt.initial(selected)
      })
      this.refreshStatus()
    }).catch(err => this.toast(err.message || '切换音色失败'))
  },

  // 固定音色条：可手指左右拖动 + 自动漂移，对齐网页 index-app.js:953-1029。
  // 展开控制台后 2.6s 开始，每 60ms 右移 1px，滚过一份音色后回到 0；按住/拖动即停，松手 1.8s 恢复。
  syncRailDrift() {
    const want = !!this.data.expanded
    if (want === this._railActive) return
    this._railActive = want
    if (want) this.startRailDrift()
    else this.stopRailDrift()
  },

  startRailDrift() {
    this.stopRailDrift()
    this._railPos = 0
    this._railLoop = 0
    this.setData({ railScroll: 0, railPaused: false })
    this._railDelay = setTimeout(() => {
      this._railDelay = null
      this.measureRailLoop()
      this._railDrift = setInterval(() => this.stepRailDrift(), 60)
    }, 2600)
  },

  stopRailDrift() {
    if (this._railDelay) { clearTimeout(this._railDelay); this._railDelay = null }
    if (this._railDrift) { clearInterval(this._railDrift); this._railDrift = null }
    if (this._railTimer) { clearTimeout(this._railTimer); this._railTimer = null }
  },

  measureRailLoop() {
    if (this._railLoop) return
    wx.createSelectorQuery().selectAll('.fixed-rail .voice-chip').boundingClientRect().exec((res) => {
      const list = (res && res[0]) || []
      const half = Math.floor(list.length / 2)
      if (half < 1 || !list[half]) return
      const delta = list[half].left - list[0].left
      if (delta > 0) this._railLoop = delta
    })
  },

  stepRailDrift() {
    if (!this.data.expanded || this.data.railPaused || !this._railLoop) return
    let next = (this._railPos || 0) + 1
    if (next >= this._railLoop) next -= this._railLoop
    this._railPos = next
    this.setData({ railScroll: next })
  },

  onRailScroll(e) {
    this._railPos = (e && e.detail && e.detail.scrollLeft) || 0
  },

  pauseRail() {
    if (this._railTimer) { clearTimeout(this._railTimer); this._railTimer = null }
    if (!this.data.railPaused) this.setData({ railPaused: true })
  },

  resumeRail() {
    if (this._railTimer) clearTimeout(this._railTimer)
    this._railTimer = setTimeout(() => {
      this._railTimer = null
      if (!this.data.expanded) return
      this.setData({ railPaused: false })
    }, 1800)
  },

  onVolumeStart(e) {
    const selector = this.data.expanded ? '.con-volume .volume' : '.sheet-main .volume'
    wx.createSelectorQuery().in(this).select(selector).boundingClientRect(rect => {
      if (rect) this._volumeRect = rect
      this.updateVolumeFromTouch(e)
    }).exec()
  },

  onVolumeMove(e) {
    this.updateVolumeFromTouch(e)
  },

  updateVolumeFromTouch(e) {
    const touch = (e.touches && e.touches[0]) || (e.changedTouches && e.changedTouches[0])
    const rect = this._volumeRect
    if (!touch || !rect || !rect.width) return
    const pct = fmt.clamp(Math.round(((touch.clientX - rect.left) / rect.width) * 100), 0, 100)
    if (pct === this.data.status.volume) return
    this.setData({ 'status.volume': pct })
    if (this._volumeTimer) clearTimeout(this._volumeTimer)
    this._volumeTimer = setTimeout(() => this.sendVolume(pct), 120)
  },

  onVolumeEnd(e) {
    const touch = (e.changedTouches && e.changedTouches[0]) || (e.touches && e.touches[0])
    if (touch) this.updateVolumeFromTouch(e)
    if (this._volumeTimer) clearTimeout(this._volumeTimer)
    this.sendVolume(this.data.status.volume)
  },

  sendVolume(volume) {
    if (!this.client) return
    this.client.request('set_volume', { volume: volume }).catch(err => {
      this.toast(err.message || '音量调整失败')
      this.refreshStatus()
    })
  },

  openWifi() {
    this.setData({
      wifiModal: true,
      wifiScanning: false,
      wifiSelected: '',
      wifiNeedPassword: false,
      wifiPassword: '',
      wifiError: '',
      wifiFocus: false,
      wifiCurrent: this.data.status.ssid || '',
      wifiCurrentSub: this.data.status.online ? (this.data.status.band + ' · 已连接') : '未连接'
    })
    this.scanWifi()
  },

  closeWifi() {
    if (this._wifiTimer) clearTimeout(this._wifiTimer)
    this.setData({ wifiModal: false, wifiConnecting: false, wifiFocus: false })
  },

  async scanWifi() {
    if (this.data.wifiScanning || !this.client) return
    this.setData({ wifiScanning: true, wifiError: '' })
    try {
      const raw = await this.client.request('wifi_scan', {}, 16000)
      const networks = Array.isArray(raw) ? raw : ((raw && raw.networks) || [])
      // Keep all reported networks visible. Prefer a connectable 2.4 GHz
      // radio for dual-band APs that share the same SSID.
      const bySsid = {}
      networks.forEach(item => {
        if (!item || !item.ssid) return
        const band = item.band || '2.4G'
        const rssi = Number(item.rssi != null ? item.rssi : -70)
        const entry = {
          ssid: item.ssid,
          band: band,
          supported: band !== '5G',
          secure: item.secure !== false,
          current: !!item.current,
          rssi: rssi,
          bars: fmt.signalBars(rssi)
        }
        const prev = bySsid[item.ssid]
        if (!prev || (entry.supported && !prev.supported) ||
          (entry.supported === prev.supported &&
            (entry.current || (!prev.current && entry.rssi > prev.rssi)))) {
          bySsid[item.ssid] = entry
        }
      })
      const list = Object.keys(bySsid).map(k => bySsid[k])
      list.sort((a, b) => (b.current ? 1 : 0) - (a.current ? 1 : 0) || b.rssi - a.rssi)
      this.setData({ wifiList: list, wifiScanning: false })
    } catch (e) {
      this.setData({ wifiScanning: false, wifiError: e.message || '扫描 Wi-Fi 失败' })
    }
  },

  selectWifi(e) {
    const ssid = e.currentTarget.dataset.ssid
    const item = this.data.wifiList.find(w => w.ssid === ssid)
    if (!item) return
    if (item.band === '5G') {
      this.setData({ wifiError: '设备仅支持 2.4GHz，请选择 2.4G 网络' })
      return
    }
    this.setData({
      wifiSelected: ssid,
      wifiNeedPassword: item.secure,
      wifiPassword: '',
      wifiFocus: item.secure,
      wifiError: ''
    })
    if (!item.secure) {
      setTimeout(() => this.setData({ wifiFocus: false }), 200)
    }
  },

  onWifiPassword(e) {
    this.setData({ wifiPassword: e.detail.value || '' })
  },

  async connectWifi() {
    if (this.data.wifiConnecting) return
    const ssid = this.data.wifiSelected
    if (!ssid) return this.setData({ wifiError: '请选择一个 Wi-Fi' })
    if (this.data.wifiNeedPassword && this.data.wifiPassword.length < 8) {
      return this.setData({ wifiError: 'Wi-Fi 密码至少 8 位' })
    }
    this.setData({ wifiConnecting: true, wifiError: '' })
    try {
      await this.client.request('wifi_connect', { ssid: ssid, password: this.data.wifiPassword })
      if (this._wifiTimer) clearTimeout(this._wifiTimer)
      // Firmware waits 20 s (40 x 500 ms) before reporting failure; keep the
      // local fallback slightly longer so its wifi_state event wins the race.
      this._wifiTimer = setTimeout(() => {
        if (this.data.wifiConnecting) {
          this.setData({ wifiConnecting: false, wifiError: '设备响应超时，请重试' })
        }
      }, 22000)
    } catch (e) {
      this.setData({ wifiConnecting: false, wifiError: e.message || '连接失败，请检查密码' })
    }
  },

  openVoiceManager() {
    this.setData({
      voiceModal: true,
      vmCurrent: this.data.status.currentVoice,
      vmCurrentInitial: fmt.initial(this.data.status.currentVoice),
      vmConfirm: false,
      vmError: '',
      editingId: ''
    })
    this.refreshVoices()
  },

  closeVoiceManager() {
    this.setData({ voiceModal: false, vmConfirm: false, editingId: '', vmError: '' })
  },

  onVmUse(e) {
    const name = e.currentTarget.dataset.name
    this.selectVoice(name)
  },

  onVmRename(e) {
    const id = e.currentTarget.dataset.id
    const name = e.currentTarget.dataset.name
    this.setData({ editingId: id, editingValue: name })
  },

  onVmRenameInput(e) {
    this.setData({ editingValue: e.detail.value || '' })
  },

  async onVmRenameDone() {
    const id = this.data.editingId
    const value = (this.data.editingValue || '').trim()
    if (!id || this._renaming) return
    if (!value) {
      this.setData({ editingId: '' })
      return
    }
    this._renaming = true
    try {
      const res = await this.client.request('voice_rename', { id: id, name: value })
      if (res && res.clones) this.applyClones(res.clones)
      else {
        const clones = this.data.clones.map(c => (c.id === id ? Object.assign({}, c, { name: value, initial: fmt.initial(value) }) : c))
        this.applyClones(clones)
        this.refreshVoices()
      }
      this.setData({ editingId: '' })
      this.toast('已重命名')
    } catch (e) {
      this.toast(e.message || '重命名失败')
    } finally {
      this._renaming = false
    }
  },

  onVmDelete(e) {
    const id = e.currentTarget.dataset.id
    const name = e.currentTarget.dataset.name
    this.setData({ vmConfirm: true, vmPendingId: id, vmPendingName: name, vmError: '' })
  },

  onVmCancel() {
    this.setData({ vmConfirm: false, vmPendingId: '', vmPendingName: '', vmError: '' })
  },

  async onVmConfirm() {
    const id = this.data.vmPendingId
    if (!id) return
    try {
      const item = this.data.clones.find(c => c.id === id)
      const wasCurrent = !!item && item.name === this.data.vmCurrent
      const res = await this.client.request('voice_delete', { id: id })
      let clones = (res && res.clones) || this.data.clones.filter(c => c.id !== id)
      this.applyClones(clones)
      if (wasCurrent) {
        // 固件删除当前音色后会切回第一个内置音色；以设备返回的 voice_list 为准。
        const fallback = this.data.fixedVoices[0] || '龙安欢'
        this.setData({ vmCurrent: fallback, vmCurrentInitial: fmt.initial(fallback), 'status.currentVoice': fallback, voiceInitial: fmt.initial(fallback) })
        this.refreshVoices()
      }
      this.setData({ vmConfirm: false, vmPendingId: '', vmPendingName: '', vmError: '' })
      this.toast('已删除')
    } catch (e) {
      this.setData({ vmError: e.message || '删除失败，请重试' })
    }
  },

  onVmNew() {
    this.setData({ voiceModal: false })
    this.openAudioModal()
  },

  openAudioModal() {
    if (this.data.audioRecordPending) return this.setData({ audioModal: true })
    this.setData({
      audioModal: true,
      audioMode: 'record',
      audioRecording: false,
      audioRecordPending: false,
      audioCreating: false,
      audioElapsed: 0,
      audioTime: '00:00',
      audioProgress: 0,
      audioDone: false,
      audioName: '我的音色',
      audioFileName: '',
      audioFileInfo: '',
      audioFile: '',
      audioError: '',
      audioHint: '请在提示后连续清晰朗读 10–15 秒，玩具最多录 20 秒。',
      audioRecordStatus: '点击“开始录制”，按提示连续说话',
      audioTrim: 0
    })
  },

  openAudioUpload() {
    if (this.data.audioRecordPending) return this.setData({ audioModal: true })
    this.setData({
      audioModal: true,
      audioMode: 'upload',
      audioRecording: false,
      audioRecordPending: false,
      audioCreating: false,
      audioElapsed: 0,
      audioTime: '00:00',
      audioProgress: 0,
      audioDone: false,
      audioName: '我的音色',
      audioFileName: '',
      audioFileInfo: '',
      audioFile: '',
      audioError: '',
      audioHint: '仅支持 WAV，5–20 秒；上传后会转成 16kHz 单声道。'
    })
  },

  switchAudioTab(e) {
    if (this.data.audioRecordPending) return this.toast('录音仍在进行，请稍候')
    const mode = e.currentTarget.dataset.mode
    this.setData({
      audioMode: mode,
      audioError: '',
      audioHint: mode === 'record'
        ? '请在提示后连续清晰朗读 10–15 秒，玩具最多录 20 秒。'
        : '仅支持 WAV，5–20 秒；上传后会转成 16kHz 单声道。'
    })
  },

  onAudioPrimary() {
    if (this.data.audioMode === 'record') this.startRecord()
    else this.createVoice()
  },

  toggleRecord() {
    this.startRecord()
  },

  startRecord() {
    if (this.data.audioRecordPending || this.data.audioRecording || this.data.audioCreating) return
    const name = (this.data.audioName || '').trim().slice(0, 8)
    if (!name) return this.setData({ audioError: '请先给音色起个名字' })
    if (!this.client) return this.setData({ audioError: '设备未连接' })
    this._recordPolling = false
    this._recordPollGeneration = (this._recordPollGeneration || 0) + 1
    this._recordPhaseRank = 0
    this.setData({
      audioRecordPending: true,
      audioRecording: false,
      audioElapsed: 0,
      audioTime: '00:00',
      audioProgress: 0,
      audioDone: false,
      audioError: '',
      audioRecordStatus: '已发送，请听玩具提示…'
    })
    this.client.request('voice_record', { name: name, statusPoll: true }).then(res => {
      if (!res || !res.accepted) throw new Error('设备没有开始录音')
      if (this.data.audioRecordPending) {
        this._recordPolling = true
        this.pollDeviceRecord()
      }
    }).catch(err => {
      const uncertain = (err && (err.code === 1009 || err.code === 1004)) ||
        (this.client && this.client.connected === false)
      this.setData({
        audioRecordPending: false,
        audioRecording: false,
        audioError: uncertain ? '请求结果未知，请重新连接并先查看音色列表' : (err.message || '录音失败，请重试'),
        audioRecordStatus: '点击“开始录制”，按提示连续说话'
      })
    })
  },

  pollDeviceRecord() {
    if (!this.client || !this._recordPolling || this._recordPollInFlight) return
    const generation = this._recordPollGeneration
    this._recordPollInFlight = true
    this.client.request('voice_record_status', {}).then(data => {
      if (generation === this._recordPollGeneration && this.data.audioRecordPending && data) {
        this.onBleEvent('voice_record', Object.assign({ name: this.data.audioName }, data))
      }
    }).catch(err => {
      if (generation !== this._recordPollGeneration || !this.data.audioRecordPending) return
      if (err && err.code === 1002) {
        // Older firmware reports progress as events and may not have this operation.
        this._recordPolling = false
      } else if ((err && err.code === 1004) || (this.client && this.client.connected === false)) {
        this.setData({ audioRecordStatus: '连接已断开，结果未知；重新连接后请先查看音色列表' })
      }
    }).then(() => { this._recordPollInFlight = false })
  },

  startRecordClock() {
    if (this._recordTimer) clearInterval(this._recordTimer)
    this._recordTimer = setInterval(() => this.tickRecord(), 100)
  },

  tickRecord() {
    const elapsed = Math.min(20, Number((this.data.audioElapsed + 0.1).toFixed(1)))
    this.setData({
      audioElapsed: elapsed,
      audioTime: fmt.formatDuration(elapsed),
      audioProgress: Math.min(99, Math.round((elapsed / 20) * 100))
    })
    if (elapsed >= 20) {
      this.stopRecordClock()
      this.setData({ audioRecordStatus: '等待玩具确认录音状态…' })
    }
  },

  stopRecordClock() {
    if (this._recordTimer) clearInterval(this._recordTimer)
    this._recordTimer = null
  },

  finishDeviceRecord(name) {
    this._recordPolling = false
    this._recordPollGeneration = (this._recordPollGeneration || 0) + 1
    this.stopRecordClock()
    this.setData({
      audioRecording: false,
      audioRecordPending: false,
      audioCreating: false,
      audioDone: true,
      audioProgress: 100,
      audioModal: false,
      audioError: '',
      audioRecordStatus: '音色已生成，可以在管理里试听或使用'
    })
    this.toast('音色“' + (name || this.data.audioName) + '”已生成')
    this.refreshVoices()
  },

  failDeviceRecord(message) {
    this._recordPolling = false
    this._recordPollGeneration = (this._recordPollGeneration || 0) + 1
    this.stopRecordClock()
    this.setData({
      audioRecording: false,
      audioRecordPending: false,
      audioCreating: false,
      audioDone: false,
      audioError: message || '录音失败，请重试',
      audioRecordStatus: '点击“开始录制”，按提示连续说话'
    })
  },

  onAudioNameInput(e) {
    this.setData({ audioName: e.detail.value || '' })
  },

  chooseAudio() {
    const done = (file) => {
      const name = file.name || file.path || '我的音频.wav'
      if (!/\.wav$/i.test(name)) {
        this.setData({ audioError: '仅支持 WAV，请先转换成 WAV 再上传' })
        return
      }
      const size = file.size ? (file.size / 1024 / 1024).toFixed(1) + ' MB' : '大小未知'
      this.setData({
        audioFile: file.path || '',
        audioFileName: name,
        audioFileInfo: '已选择 · ' + size + ' · 生成时校验时长',
        audioDone: true,
        audioError: ''
      })
    }
    if (wx.chooseMessageFile) {
      wx.chooseMessageFile({
        count: 1,
        type: 'file',
        extension: ['wav'],
        success: res => {
          const file = (res.tempFiles && res.tempFiles[0]) || {}
          done(file)
        },
        fail: err => {
          if (this.mock) {
            done({ name: 'sample.wav', size: 352844, path: 'mock://sample.wav' })
          } else if (!/cancel/.test((err && err.errMsg) || '')) {
            this.setData({ audioError: '选择文件失败，请重试' })
          }
        }
      })
    } else if (this.mock) {
      done({ name: 'sample.wav', size: 352844, path: 'mock://sample.wav' })
    } else {
      this.setData({ audioError: '当前微信版本不支持选择文件' })
    }
  },

  onTrimChange(e) {
    this.setData({ audioTrim: Number(e.detail.value || 0) })
  },

  async createVoice() {
    if (this.data.audioMode === 'record') return this.startRecord()
    const name = (this.data.audioName || '').trim().slice(0, 8)
    if (!name) return this.setData({ audioError: '请给音色起个名字' })
    if (!this.data.audioDone) return this.setData({ audioError: '请先选择 WAV 文件' })
    if (!this.client) return this.setData({ audioError: '设备未连接' })
    if (this.data.audioCreating) return
    this.setData({ audioCreating: true, audioError: '', audioProgress: 0, audioRecordStatus: '正在读取 WAV…' })
    try {
      let pcm = null
      if (this.mock) {
        pcm = audio.mockPcm(6)
      } else {
        const buffer = await audio.readFile(this.data.audioFile)
        pcm = audio.wavToPcm(buffer)
      }
      this.setData({
        audioFileInfo: pcm.duration.toFixed(1) + ' 秒 · 16kHz 单声道',
        audioRecordStatus: '正在上传到玩具…'
      })
      await audio.uploadPcm(this.client, pcm, name, progress => {
        const pct = Math.round(progress * 100)
        this.setData({ audioProgress: pct, audioRecordStatus: '正在上传 ' + pct + '%' })
      })
      this.setData({ audioModal: false, audioCreating: false, audioRecording: false })
      this.toast('音色“' + name + '”已生成')
      this.refreshVoices()
    } catch (e) {
      this.setData({ audioCreating: false, audioError: e.message || '生成失败，请重试' })
    }
  },

  cancelAudio() {
    if (this.data.audioCreating) return this.toast('正在生成，请稍候')
    if (this.data.audioRecordPending) return this.setData({ audioModal: false })
    this.stopRecordClock()
    this.setData({ audioModal: false, audioRecording: false, audioRecordPending: false })
  },

  openReset() {
    this.setData({ resetModal: true, resetInput: '', resetReady: false, resetError: '', resetStage: '', resetting: false })
  },

  closeReset() {
    if (this.data.resetting) return
    this.setData({ resetModal: false })
  },

  onResetInput(e) {
    const value = e.detail.value || ''
    this.setData({ resetInput: value, resetReady: value === '确定恢复出厂设置' })
  },

  async confirmReset() {
    if (!this.data.resetReady || this.data.resetting) return
    this.setData({ resetting: true, resetError: '', resetStage: '正在校验设备状态…' })
    let timeoutId = null
    try {
      const done = new Promise(resolve => { this._resetDoneResolve = resolve })
      await this.client.request('reset_factory', {})
      this.setData({ resetStage: '已提交，等待设备确认…' })
      const outcome = await Promise.race([
        done,
        new Promise(resolve => {
          timeoutId = setTimeout(() => resolve({ ok: false, timeout: true }), 30000)
        })
      ])
      if (!outcome || !outcome.ok) {
        throw new Error(outcome && outcome.timeout ? '设备未确认恢复完成，请重试' : (outcome.message || '恢复失败，请重试'))
      }
      this._leaving = true
      this.setData({ resetModal: false, resetting: false, resetStage: '' })
      this.toast('已恢复出厂设置')
      const app = getApp()
      if (app.clearBle) app.clearBle()
      try { if (this.client) this.client.close() } catch (e) {}
      setTimeout(() => wx.redirectTo({ url: '/pages/connect/connect' }), 1500)
    } catch (e) {
      this.setData({ resetting: false, resetStage: '', resetError: e.message || '恢复失败，请重试' })
    } finally {
      if (timeoutId) clearTimeout(timeoutId)
      this._resetDoneResolve = null
    }
  },

  applyAppearance(patch) {
    const next = store.setTheme(patch, this.data.theme)
    try { getApp().globalData.theme = next } catch (e) {}
    this.setData({
      theme: next.theme,
      material: next.material,
      layout: next.layout,
      mode: next.mode,
      themeLabel: THEME_LABEL[next.theme] || '奶糖',
      materialLabel: MATERIAL_LABEL[next.material] || '奶润'
    })
  },

  onTheme(e) { this.applyAppearance({ theme: e.currentTarget.dataset.theme }) },
  onMaterial(e) { this.applyAppearance({ material: e.currentTarget.dataset.mat }) },
  onLayout(e) { this.applyAppearance({ layout: e.currentTarget.dataset.layout }) },
  onMode(e) { this.applyAppearance({ mode: e.currentTarget.dataset.mode }) },

  toast(text) {
    this.setData({ toastShow: true, toastText: text })
    if (this._toastTimer) clearTimeout(this._toastTimer)
    this._toastTimer = setTimeout(() => this.setData({ toastShow: false }), 2200)
  },

  onShareAppMessage() {
    return {
      title: this.data.product.name + ' · ' + this.data.product.english,
      path: '/pages/connect/connect'
    }
  }
})
