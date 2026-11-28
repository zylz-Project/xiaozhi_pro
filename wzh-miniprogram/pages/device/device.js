const config = require('../../services/config')
const MockClient = require('../../services/mock')
const products = require('../../services/products')
const store = require('../../services/store')
const fmt = require('../../utils/format')
const audio = require('../../services/audio')
const phoneRecorder = require('../../services/phone_recorder')

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
  { target: '.sheet-main .primary', title: '和小熊猫打声招呼', text: '点「开始对话」，听听它联网后会说什么。', state: 'collapsed' },
  { target: '.voice-row', title: '声音都在这里', text: '点一下，打开完整控制台。', state: 'collapsed' },
  { target: '.con-head', title: '这是控制台', text: '设备、声音、网络和说明书都在这里。', state: 'expanded' },
  { target: '.network-tile', title: '网络和信号', text: '点“配置”可以连接或更换 Wi-Fi。', state: 'expanded', panel: 'panel-device' },
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
const ORB_NOTICE = {
  '电池电量不足，暂时无法活动，请先充电。': '我的电量有点低，先帮我充会儿电吧。',
  '设备温度过高，暂时无法活动，请等待冷却。': '我有点热，等我凉一会儿再玩吧。',
  '动作暂时受保护，请将设备平稳放置后重试。': '我先歇一下，把我放平就好啦。',
  '正在检查电池状态，动作暂未启动，请稍候。': '我在看看还有多少电，等我一下。'
}

const WIFI_GUIDE_STEPS = [
  { target: '.handle', title: '展开详情栏', text: '点底部的拉手，打开设备控制台。' },
  { target: '.net-change', title: '点击配置', text: '在网络卡片上打开配置。', panel: 'panel-device' },
  { target: '.wifi-list', viewport: '.wifi-modal .modal-body', title: '选择网络', text: '从附近网络中选一个 2.4 GHz Wi-Fi。' },
  { target: '.wifi-password', title: '输入密码', text: '输完 Wi-Fi 密码后，点键盘上的完成或收起键盘。' },
  { target: '.wifi-modal .modal-foot .primary', title: '点击连接', text: '点“连接网络”，等待设备确认。' }
]
const NETWORK_GUIDE_STEP = {
  target: '.wifi-options',
  viewport: '.wifi-modal .modal-body',
  title: '选择 Wi-Fi 或 4G',
  text: '选择附近的 Wi-Fi，或点 4G 使用移动网络。4G 不用输密码。'
}
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
    intro: '一只会转头、会挥爪、会听你说话的桌面小伙伴。第一次使用，从这里开始。',
    partsIntro: '先认识它的六个部分。日常操作只会用到电源键、BOOT 键和头顶触摸区。',
    parts: [
      { n: 'A', t: '头部', d: '左右转头、看看四周' },
      { n: 'B', t: '触摸区', d: '头顶，轻摸会回应' },
      { n: 'C', t: '电源键', d: '双击对话，长按开关机' },
      { n: 'D', t: 'BOOT 键', d: '双击开始或退出对话' },
      { n: 'E', t: '双前肢', d: '会挥爪、会撑起身体' },
      { n: 'F', t: '躯干与底座', d: '扬声器、麦克风、USB-C 充电口' }
    ],
    motionIntro: '它用头部和双前肢表达情绪。动作会根据声音大小、触摸和对话状态变化。',
    motionList: [
      { t: '待机', d: '轻轻呼吸，偶尔转头看看四周。' },
      { t: '对话', d: '听到声音时转头，前肢随语音包络摆动。' },
      { t: '亲昵', d: '轻摸头顶，转头看你并轻轻挥爪。' },
      { t: '开心', d: '同时摸两个触摸区，动作幅度更大。' },
      { t: '说话', d: '回答时头部跟随语音节奏转动。' }
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
      { n: 'A', t: '头部', d: '左右转头、看看四周' },
      { n: 'B', t: '触摸区', d: '头顶，轻摸会回应' },
      { n: 'C', t: '电源键', d: '双击对话，长按开关机' },
      { n: 'D', t: 'BOOT 键', d: '双击开始或退出对话' },
      { n: 'E', t: '颈部', d: '会前伸、后仰，凑近看你' },
      { n: 'F', t: '尾巴与躯干', d: '尾巴左右摆，躯干内有扬声器和充电口' }
    ],
    motionIntro: '它用颈部、头部和尾巴表达情绪。动作会根据声音大小、触摸和对话状态变化。',
    motionList: [
      { t: '待机', d: '慢慢呼吸，尾巴偶尔扫一下。' },
      { t: '对话', d: '听到声音时颈部前伸，尾巴跟着语音包络摆动。' },
      { t: '亲昵', d: '轻摸头顶，转头看你并轻轻摆尾。' },
      { t: '开心', d: '同时摸两个触摸区，颈部与尾巴动作更大。' },
      { t: '说话', d: '回答时颈部和尾巴跟随语音节奏动作。' }
    ],
    inlineBody: '颈部、尾巴',
    inlineChat: '颈部和尾巴',
    inlineWarn: '颈部、尾巴',
    specAction: '颈部 2 路 + 头部 1 路 + 尾巴 2 路舵机'
  },
  redpanda: {
    intro: '一只会转头、会摇大尾巴、会听你说话的桌面小伙伴。第一次使用，从这里开始。',
    partsIntro: '先认识它的六个部分。日常操作只会用到电源键、BOOT 键和头顶触摸区。',
    parts: [
      { n: 'A', t: '头部', d: '左右转头、看看四周' },
      { n: 'B', t: '触摸区', d: '头顶，轻摸会回应' },
      { n: 'C', t: '电源键', d: '双击对话，长按开关机' },
      { n: 'D', t: 'BOOT 键', d: '双击开始或退出对话' },
      { n: 'E', t: '大尾巴', d: '会左右摇，休息时盘在身后' },
      { n: 'F', t: '躯干与底座', d: '扬声器、麦克风、USB-C 充电口' }
    ],
    motionIntro: '它用头部和大尾巴表达情绪。动作会根据声音大小、触摸和对话状态变化。',
    motionList: [
      { t: '待机', d: '轻轻呼吸，大尾巴慢慢晃。' },
      { t: '对话', d: '听到声音时转头，尾巴跟着语音包络摇动。' },
      { t: '亲昵', d: '轻摸头顶，转头看你并轻轻摇尾。' },
      { t: '开心', d: '同时摸两个触摸区，头部与尾巴动作更大。' },
      { t: '说话', d: '回答时头部和尾巴跟随语音节奏动作。' }
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
  const devicePrefix = product.ap + '-XXXX'
  return [
    { q: '小程序搜不到设备怎么办？', a: '确认玩具已开机、手机蓝牙已开启，并且距离在 1 米内。设备名称通常以 ' + devicePrefix + ' 的形式显示，后四位因设备而异；若仍搜不到，长按电源键关机再开机，然后在连接页点“重新扫描”。' },
    { q: '连接后很快断开？', a: '先确认手机没有同时连接其他蓝牙音频设备，并把手机靠近玩具，避免隔着墙或金属桌面。iOS 需要允许微信使用蓝牙权限。' },
    { q: '连不上家里的 Wi-Fi？', a: '设备支持 2.4GHz Wi-Fi。请确认路由器已开启 2.4GHz 和 DHCP，且没有开启 MAC 地址过滤；加密网络请检查密码。' },
    { q: '设备离线还能用吗？', a: '可以。离线时仍能用蓝牙连接小程序查看状态和更改网络；云端对话和复刻音色需要设备联网。有待清理的云端音色时，恢复出厂也需要联网。' },
    { q: '复刻音色失败？', a: '确认设备在线、环境安静、一个人连续说话 5–20 秒。提示“复刻音色已满”时，先在音色管理里删除一个再试。' },
    { q: '触摸没有反应？', a: '对话进行中触摸只驱动动作，不额外播放声音。若完全没反应，确认没有长时间按住多个触摸区，或重启设备再试。' },
    { q: '设备发热正常吗？', a: '舵机连续动作或边充边用会发热，属于正常现象。温度过高时设备会限制动作；请放在通风处，避免阳光直射。' },
    { q: '双击电源键没反应？', a: '确认电量充足、设备已开机。正在说话时双击会结束本轮对话，等提示音结束后再双击开始。' }
  ]
}

const DEFAULT_STATUS = {
  online: true,
  state: 'idle',
  startupNotice: '',
  onboarding: null,
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

function safeAreaBottom(info) {
  const h = Number(info.windowHeight) || 0
  const safe = info.safeArea
  if (!h || !safe || typeof safe.bottom !== 'number') return 0
  return Math.max(0, Math.round(h - safe.bottom))
}

function pad(n) { return n < 10 ? '0' + n : '' + n }

function stateCopy(online, state) {
  if (!online) return { sheet: '离线', cta: '设备离线', disabled: true }
  switch (state) {
    case 'listening': return { sheet: '聆听中', cta: '结束对话', disabled: false }
    case 'speaking': return { sheet: '说话中', cta: '结束对话', disabled: false }
    case 'activating': return { sheet: '正在联网', cta: '服务启动中…', disabled: true }
    case 'connecting': return { sheet: '连接中', cta: '连接中…', disabled: true }
    case 'offline': return { sheet: '离线', cta: '设备离线', disabled: true }
    default: return { sheet: '待机', cta: '开始对话', disabled: false }
  }
}

Page({
  data: {
    statusBarHeight: 20,
    menuBottom: 86,
    safeBottom: 0,
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
    vmPrevious: '',
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
    audioPhonePending: false,
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
    wifiSelectedTransport: '',
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
    guideKind: '',
    guideStep: 0,
    guideSaving: false,
    guideDots: GUIDE_STEPS.map(function (_, i) { return i }),
    guideCurrent: { title: '', text: '' },
    guideSpot: { left: 0, top: 0, width: 0, height: 0, radius: 18 },
    guideBubble: { place: 'below', style: 'opacity:0' }
  },

  onReady() {
    this.syncSafeBottom()
    this._guideReady = true
    this.scheduleFirstGuide()
  },

  /* The simulator/WeChat may report the window before the device safe area is
     applied, so recompute once the page is laid out. */
  syncSafeBottom() {
    const info = windowInfo()
    const sb = safeAreaBottom(info)
    if (sb === this.data.safeBottom) return
    const patch = { safeBottom: sb }
    if (!this._assistantCustom) {
      const winW = Number(info.windowWidth) || 390
      const winH = Number(info.windowHeight) || 844
      const faSize = winW * 100 / 750
      const faMargin = winW * 26.92 / 750
      patch.assistantY = Math.max((info.statusBarHeight || 20) + 8, winH - faSize - faMargin - sb)
    }
    this.setData(patch)
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
    this._auditionRequest = 0
    this._wifiTimer = null
    this._wifiScanRevision = 0
    this._statusRevision = 0
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
    const safeBottom = safeAreaBottom(info)
    this._assistantCustom = false
    const assistantX = Math.max(8, winW - faSize - faMargin)
    const assistantY = Math.max((info.statusBarHeight || 20) + 8, winH - faSize - faMargin - safeBottom)

    this.setData({
      statusBarHeight: info.statusBarHeight || 20,
      menuBottom: menuBottom,
      safeBottom: safeBottom,
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
    this._guidePaused = false
    this.syncSafeBottom()
    if (this.client) {
      this.refreshStatus()
      this.startStatusTimer()
    }
    this.scheduleFirstGuide()
  },

  onHide() {
    this._guidePaused = true
    this.cancelPhoneRecording()
    if (this._guideAutoTimer) clearTimeout(this._guideAutoTimer)
    this._guideAutoTimer = null
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
    this.cancelPhoneRecording()
    if (this._guideAutoTimer) clearTimeout(this._guideAutoTimer)
    this._guideAutoTimer = null
    this._unloaded = true
    this.stopStatusTimer()
    this.stopRailDrift()
    if (this._recordTimer) clearInterval(this._recordTimer)
    if (this._volumeTimer) clearTimeout(this._volumeTimer)
    if (this._toastTimer) clearTimeout(this._toastTimer)
    if (this._auditionTimer) clearTimeout(this._auditionTimer)
    this._auditionRequest = 0
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
    const guide = s.onboarding && typeof s.onboarding === 'object' ? s.onboarding : null
    const wifi = raw && raw.wifi
    const online = typeof raw?.online === 'boolean' ? raw.online
      : typeof wifi?.connected === 'boolean' ? wifi.connected : s.online !== false
    const transport = s.transport === 'cellular' ? 'cellular' : 'wifi'
    const cellularAvailable = typeof raw?.cellular_available === 'boolean'
      ? raw.cellular_available : (raw?.cellularAvailable ?? base.cellularAvailable) === true
    const wifiConfigured = typeof wifi?.configured === 'boolean' ? wifi.configured
      : (s.wifiConfigured || (online && transport === 'wifi'))
    return {
      online,
      transport,
      cellularAvailable,
      networkConfigured: typeof raw?.network_configured === 'boolean' ? raw.network_configured
        : (raw?.networkConfigured ?? (transport === 'cellular'
          ? (base.networkConfigured || online) : wifiConfigured)),
      wifiConfigured,
      guide_done: s.guide_done === true ? true : s.guide_done === false ? false : null,
      state: s.state || 'idle',
      startupNotice: typeof s.startup_notice === 'string'
        ? s.startup_notice
        : (typeof s.startupNotice === 'string' ? s.startupNotice : ''),
      onboarding: guide ? {
        done: guide.done === true || guide.done === 1,
        blocked: guide.blocked === true || guide.blocked === 1
      } : null,
      battery: typeof s.battery === 'number' ? fmt.clamp(s.battery, 0, 100) : null,
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
    const baseHasCellular = this.data.status.cellularAvailable
    const s = this.normalizeStatus(raw)
    const copy = stateCopy(s.online, s.state)
    const app = getApp()
    if (app && app.globalData) app.globalData.lastStatus = s
    if (raw && (typeof raw.online === 'boolean' || typeof raw.wifi?.connected === 'boolean')) this._guideStatusReady = true
    // A completion already acknowledged by this device cannot become pending
    // again within the same BLE session, even if an older status arrives late.
    if (this._guideDone) s.guide_done = true
    else if (s.guide_done === true) this._guideDone = true
    this.setData({
      status: s,
      orbNotice: ORB_NOTICE[s.startupNotice] || s.startupNotice,
      deviceState: s.online ? s.state : 'offline',
      sheetState: copy.sheet,
      ctaLabel: s.onboarding && s.onboarding.blocked ? '请先完成使用教程' : copy.cta,
      ctaDisabled: copy.disabled || !!(s.onboarding && s.onboarding.blocked),
      voiceInitial: fmt.initial(s.currentVoice),
      vmCurrent: s.currentVoice,
      vmCurrentInitial: fmt.initial(s.currentVoice),
      wifiCurrent: s.transport === 'cellular' ? '4G' : (s.ssid || ''),
      wifiCurrentSub: s.online ? (s.transport === 'cellular' ? '移动网络 · 已连接'
        : s.band + ' · 已连接') : '未连接',
      // 状态球跟着设备的真实状态与音量走，而不是固定节奏地“演”。
      orbVars: this.orbVars(s)
    }, () => {
      if (raw?.wifi?.provisioning === false && s.online && s.networkConfigured &&
          this.isNetworkTarget(s.transport, s.ssid)) {
        this.finishWifiSetup(s.ssid, s.transport)
      } else if (this.data.wifiModal && !this.data.wifiSelected &&
          s.cellularAvailable && !baseHasCellular) {
        this.advanceWifiGuide(2, true)
      } else {
        this.scheduleFirstGuide()
      }
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
    const epoch = Number.isInteger(dev.guide_epoch) ? dev.guide_epoch : null
    const identity = dev.identity || this.client?.deviceId
    if (this._guideEpoch !== epoch || this._guideIdentity !== identity) {
      this._guideDone = false
      this._guideFirstChat = false
      this._guideEpoch = epoch
      this._guideIdentity = identity
    }
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
    this.setData({ vmPrevious: payload.previousVoiceId || '' })
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
    const revision = this._statusRevision
    try {
      const status = await this.client.request('get_status')
      if (status && !this._leaving && revision === this._statusRevision) {
        this.applyStatus(Object.assign({}, status, { latency: this.data.status.latency }))
        const rtt = await this.measureLatency()
        if (rtt > 0 && !this._leaving && revision === this._statusRevision) {
          this.setData({ 'status.latency': rtt, 'status.latencyText': fmt.formatLatency(rtt) })
        }
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
      const transport = data.transport || 'wifi'
      if (data.state === 'connected') {
        this.finishWifiSetup(data.ssid, transport)
      } else if (data.state === 'failed' && this.isNetworkTarget(transport, data.ssid)) {
        clearTimeout(this._wifiTimer)
        this._wifiTarget = ''
        this.setData({ wifiConnecting: false, wifiError: data.message || (transport === 'cellular'
          ? '4G 连接失败，请重试' : '连接失败，请检查密码') })
        this.advanceWifiGuide(this.data.wifiNeedPassword ? 3 : 4, true)
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
        this.setData({ audioRecording: false, audioRecordCancellable: false, audioRecordStatus: '录音完成，正在处理…', audioProgress: 100,
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
      } else if (phase === 'clearing_onboarding') {
        this.setData({ resetStage: '正在清除设备引导进度…' })
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
        this._resetDoneResolve({ ok: false, message: '蓝牙已断开，恢复结果未确认。请重新连接查看。' })
        this._resetDoneResolve = null
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
    const maxY = Math.max(minY, winH - size - 8 - (this.data.safeBottom || 0))
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
    if (moved) this._assistantCustom = true
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

  async chooseToy(e) {
    const id = e.currentTarget.dataset.id
    if (!id || this._leaving) return
    store.setToy(id)
    this.setData({ toyPickerOpen: false })
    this._leaving = true
    if (this.client) { try { await this.client.close() } catch (err) {} }
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
      if (expanded) this.advanceWifiGuide(1)
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


  scheduleFirstGuide() {
    if (!this._guideReady || !this._guideStatusReady || !this.client || this._leaving ||
        this._guidePaused || this._guideAutoTimer || this.data.guideOpen ||
        this.data.wifiModal || this.data.wifiConnecting) return
    const pending = this.firstGuideKind()
    if (!pending) return
    this._guideAutoTimer = setTimeout(() => {
      this._guideAutoTimer = null
      if (this._leaving || this._guidePaused || this.data.guideOpen || this.data.wifiModal ||
          this.data.resetModal || this.data.audioModal || this.data.manualOpen) return
      const kind = this.firstGuideKind()
      if (kind) this.startDeviceGuide(kind, kind === 'wifi' && this.data.expanded ? 1 : 0)
    }, 350)
  },

  firstGuideKind() {
    const status = this.data.status
    if (!(status.networkConfigured ?? status.wifiConfigured) && !status.online) return 'wifi'
    if (!status.online || this._guideDone || status.guide_done === true) return ''
    // Older firmwares only report whether their first-use narration is complete.
    const unfinished = status.guide_done === false || (status.onboarding && !status.onboarding.done)
    return unfinished ? 'first' : ''
  },

  openGuide() {
    this.startDeviceGuide('replay', 0)
  },

  guideSteps(kind = this.data.guideKind) {
    const wifi = kind === 'wifi'
    if (!this.data.status.cellularAvailable) return wifi ? WIFI_GUIDE_STEPS : GUIDE_STEPS
    if (wifi) return WIFI_GUIDE_STEPS.map((step, i) => i === 2 ? NETWORK_GUIDE_STEP : step)
    return GUIDE_STEPS.map((step, i) => i === 5
      ? Object.assign({}, step, { text: '点“配置”可以连接或切换 Wi-Fi / 4G。' }) : step)
  },

  startDeviceGuide(kind, index) {
    const steps = this.guideSteps(kind)
    this._guidePending = null
    this._guideRevision = (this._guideRevision || 0) + 1
    this.setData({
      guideOpen: true,
      guideKind: kind,
      guideDots: steps.map((_, i) => i),
      guideStep: index,
      guideSaving: false,
      guideCurrent: { title: steps[index].title, text: steps[index].text },
      guideSpot: { left: 0, top: 0, width: 0, height: 0, radius: 18 },
      guideBubble: { place: 'below', style: 'opacity:0' },
      guideHide: false,
      manualOpen: false,
      expanded: kind === 'wifi' && index > 0,
      scrollIntoView: ''
    }, () => { this.syncRailDrift(); this.showGuideStep(index) })
  },

  showGuideStep(index) {
    const step = this.guideSteps()[index]
    if (!step) return this.closeGuide()
    // 等上一步测量完成再切换，避免卡片/聚光灯停在旧位置造成残影。
    if (this._guidePending !== undefined && this._guidePending !== null) return
    this._guidePending = index
    const revision = this._guideRevision
    const patch = { scrollIntoView: step.panel || '', guideHide: true }
    if (step.state === 'expanded') patch.expanded = true
    if (step.state === 'collapsed') patch.expanded = false
    if (step.manual === true) patch.manualOpen = true
    if (step.manual === false) patch.manualOpen = false
    this.setData(patch, () => {
      this.syncRailDrift()
      setTimeout(() => this.measureGuide(index, revision), step.state === 'expanded' ? 560 : 380)
    })
  },

  measureGuide(index, revision) {
    if (revision !== this._guideRevision) return
    const step = this.guideSteps()[index]
    if (!this.data.guideOpen || !step) { this._guidePending = null; return }
    const query = wx.createSelectorQuery()
    query.select('.phone').boundingClientRect()
    query.select(step.target).boundingClientRect()
    if (step.viewport) query.select(step.viewport).boundingClientRect()
    query.exec((res) => {
      if (revision !== this._guideRevision) return
      if (!this.data.guideOpen) { this._guidePending = null; return }
      const phone = res && res[0]
      const rect = res && res[1]
      const viewport = (res && res[2]) || phone
      const vw = (phone && phone.width) || 390
      const vh = (phone && phone.height) || 700
      const originX = (phone && phone.left) || 0
      const originY = (phone && phone.top) || 0
      const leftEdge = rect ? Math.max(0, rect.left - originX - 5) : 0
      const topEdge = rect ? Math.max(0, rect.top - originY - 5, (viewport?.top || 0) - originY) : 0
      const bottomEdge = rect ? Math.min(vh, rect.top + rect.height - originY + 5,
        viewport?.bottom == null ? vh : viewport.bottom - originY) : 0
      const spot = {
        left: leftEdge,
        top: topEdge,
        width: rect ? Math.max(0, Math.min(vw - leftEdge, rect.width + 10)) : 0,
        height: Math.max(0, bottomEdge - topEdge),
        radius: 12
      }
      const hasSpot = spot.width > 0 && spot.height > 0
      const bw = Math.min(304, vw - 32)
      const topSpace = spot.top
      const bottomSpace = vh - bottomEdge
      const below = bottomSpace >= 176 || bottomSpace >= topSpace
      const center = hasSpot ? spot.left + spot.width / 2 : vw / 2
      const left = Math.max(16, Math.min(center - bw / 2, vw - bw - 16))
      const arrow = Math.max(18, Math.min(center - left, bw - 18))
      const placement = !hasSpot ? 'top:' + Math.max(24, Math.floor(vh * 0.20)) + 'px'
        : below ? 'top:' + (bottomEdge + 10) + 'px'
          : 'bottom:' + (vh - spot.top + 10) + 'px'
      const style = 'left:' + left + 'px;width:' + bw + 'px;--pg-arrow:' + arrow + 'px;' + placement
      this._guidePending = null
      this.setData({
        guideStep: index,
        guideCurrent: { title: step.title, text: step.text },
        guideSpot: { left: spot.left, top: spot.top, width: spot.width, height: spot.height, radius: spot.radius },
        guideBubble: { place: !hasSpot ? '' : below ? 'below' : 'above', style: style },
        guideHide: false
      })
    })
  },

  nextGuide() {
    if (this._guidePending != null || this.data.guideSaving) return
    if (this.data.guideKind === 'wifi' || this.waitingForFirstChat()) return
    const next = this.data.guideStep + 1
    if (next >= GUIDE_STEPS.length) return this.closeGuide()
    this.showGuideStep(next)
  },

  async closeGuide() {
    const kind = this.data.guideKind
    if (this.data.guideSaving || !this.data.guideOpen) return
    if (kind === 'first' && this.data.status.guide_done === false && Number.isInteger(this._guideEpoch)) {
      this.setData({ guideSaving: true })
      try {
        await this.client.request('guide_complete', { epoch: this._guideEpoch })
        if (this._unloaded || this._leaving) return
        this._guideDone = true
        this.setData({ 'status.guide_done': true })
      } catch (e) {
        if (this._unloaded || this._leaving) return
        this.setData({ guideSaving: false })
        this.toast(e.message || '保存使用引导失败，请重试')
        this.refreshStatus()
        return
      }
    }
    if (kind !== 'replay') this._guidePaused = true
    this.dismissGuide(kind !== 'wifi')
  },

  dismissGuide(collapse = false) {
    this._guideRevision = (this._guideRevision || 0) + 1
    this._guidePending = null
    const patch = { guideOpen: false, guideKind: '', guideSaving: false, manualOpen: false, scrollIntoView: '' }
    if (collapse) patch.expanded = false
    this.setData(patch, () => this.syncRailDrift())
  },

  waitingForFirstChat() {
    const s = this.data.status
    return this.data.guideKind === 'first' && this.data.guideStep === 2 &&
      !this._guideFirstChat && s.onboarding && !s.onboarding.done &&
      (s.state !== 'listening' && s.state !== 'speaking')
  },

  advanceWifiGuide(index, rewind = false) {
    if (this.data.guideKind !== 'wifi' || (!rewind && index <= (this._guidePending ?? this.data.guideStep))) return
    this._guideRevision++
    this._guidePending = null
    this.showGuideStep(index)
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
    const firstChat = idle && this.waitingForFirstChat()
    const next = idle ? 'connecting' : 'idle'
    const prev = this.data.status.state
    this.applyStatus(Object.assign({}, this.data.status, { state: next }))
    if (!this.client) return
    this.client.request(idle ? 'chat_start' : 'chat_stop', {}).then(() => {
      if (firstChat && this.data.guideKind === 'first' && this.data.guideStep === 2) {
        this._guideFirstChat = true
        this.showGuideStep(3)
      }
      setTimeout(() => this.refreshStatus(), 700)
    }).catch(err => {
      this.applyStatus(Object.assign({}, this.data.status, { state: prev }))
      this.toast(err.message || '设备暂不支持语音会话控制')
    })
  },

  onTryVoice() {
    const client = this.client
    if (!client) return this.toast('设备未连接')
    if (this.data.auditioning) {
      if (!this._auditionRequest) return
      client.request('audition_stop', { request: this._auditionRequest }).catch(err => {
        if (!this._leaving && this.data.auditioning) this.toast(err.message || '停止试听失败')
      })
      return
    }
    const name = this.data.status.currentVoice
    if (name === '小智原声') {
      return this.toast('开始对话即可听到小智原声')
    }
    this.setData({ auditioning: true })
    client.request('audition', { name: name }).then(result => {
      if (this._leaving || client !== this.client) return
      this.toast('正在试听 · ' + name)
      if (!result || !result.request) {
        this._auditionTimer = setTimeout(() => this.finishAudition(), 1600)
        return
      }
      this._auditionRequest = result.request
      this.pollAudition(client, result.request, Date.now() + 60000)
    }).catch(err => {
      if (!this._leaving) this.finishAudition(err.message || '试听失败')
    })
  },

  pollAudition(client, request, deadline) {
    if (this._leaving || client !== this.client || this._auditionRequest !== request) return
    if (Date.now() >= deadline) return this.finishAudition('未收到试听结果')
    client.request('audition_status', {}).then(result => {
      if (this._leaving || this._auditionRequest !== request) return
      if (result.request !== request) return this.finishAudition('试听已结束')
      if (result.status === 'complete') return this.finishAudition()
      if (result.status === 'cancelled') return this.finishAudition('已停止试听')
      if (result.status === 'failed') return this.finishAudition('试听失败，请重试')
      this._auditionTimer = setTimeout(() => this.pollAudition(client, request, deadline), 800)
    }).catch(() => {
      if (!this._leaving && this._auditionRequest === request) this.finishAudition('未收到试听结果')
    })
  },

  finishAudition(message) {
    this._auditionRequest = 0
    this.setData({ auditioning: false })
    if (message) this.toast(message)
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
    this._wifiKeyboardShown = false
    this.setData({
      wifiModal: true,
      wifiScanning: false,
      wifiList: [],
      guideHide: this.data.guideKind === 'wifi',
      wifiSelected: '',
      wifiSelectedTransport: '',
      wifiNeedPassword: false,
      wifiPassword: '',
      wifiError: '',
      wifiFocus: false
    }, () => {
      if (this.data.status.cellularAvailable) this.advanceWifiGuide(2, true)
      this.scanWifi()
    })
  },

  closeWifi() {
    if (this.data.wifiConnecting) return
    this._wifiKeyboardShown = false
    this._wifiScanRevision = (this._wifiScanRevision || 0) + 1
    this.setData({ wifiModal: false, wifiScanning: false, wifiFocus: false }, () => {
      this.advanceWifiGuide(1, true)
      this.scheduleFirstGuide()
    })
  },

  isNetworkTarget(transport, ssid) {
    return !!this._wifiTarget && (this._wifiTargetTransport || 'wifi') === transport &&
      (transport === 'cellular' || ssid === this._wifiTarget)
  },

  finishWifiSetup(ssid, transport = 'wifi') {
    if (!this.isNetworkTarget(transport, ssid || this._wifiTarget)) return
    const cellular = transport === 'cellular'
    ssid = cellular ? '4G' : (ssid || this._wifiTarget)
    const firstSetup = this.data.guideKind === 'wifi' || this._wifiFirstSetup
    this._wifiTarget = ''
    this._statusRevision = (this._statusRevision || 0) + 1
    this._wifiScanRevision = (this._wifiScanRevision || 0) + 1
    clearTimeout(this._wifiTimer)
    if (firstSetup) this._guidePaused = false
    if (this.data.guideKind === 'wifi') this.dismissGuide(true)
    this.setData({
      wifiConnecting: false,
      wifiScanning: false,
      wifiModal: false,
      wifiSelected: '',
      wifiPassword: '',
      wifiFocus: false,
      wifiError: '',
      expanded: firstSetup ? false : this.data.expanded
    }, () => {
      const status = { online: true, transport, network_configured: true, ssid: cellular ? '' : ssid }
      if (!cellular) Object.assign(status, { wifi: { configured: true }, band: '2.4G' })
      this.applyStatus(status)
      this.refreshStatus()
    })
    this.toast('网络已连接 · ' + ssid)
  },

  async scanWifi() {
    if (this.data.wifiScanning || this.data.wifiConnecting || !this.client) return
    const revision = (this._wifiScanRevision || 0) + 1
    this._wifiScanRevision = revision
    this.setData({ wifiScanning: true, wifiError: '' })
    try {
      const raw = await this.client.request('wifi_scan', {}, 16000)
      if (this._unloaded || revision !== this._wifiScanRevision) return
      const networks = Array.isArray(raw) ? raw : ((raw && raw.networks) || [])
      // Keep all reported networks visible. Prefer a connectable 2.4 GHz
      // radio for dual-band APs that share the same SSID.
      const bySsid = Object.create(null)
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
      this.setData({ wifiList: list, wifiScanning: false }, () => {
        if (this.data.wifiModal && list.some(item => item.supported) && !this.data.wifiSelected && !this.data.status.cellularAvailable) {
          this.advanceWifiGuide(2, true)
        }
      })
    } catch (e) {
      if (this._unloaded || revision !== this._wifiScanRevision) return
      this.setData({ wifiScanning: false, wifiError: e.message || '扫描 Wi-Fi 失败' })
    }
  },

  selectWifi(e) {
    const ssid = e.currentTarget.dataset.ssid
    const item = this.data.wifiList.find(w => w.ssid === ssid)
    if (!item || this.data.wifiConnecting) return
    if (item.band === '5G') {
      this.setData({ wifiError: '设备仅支持 2.4GHz，请选择 2.4G 网络' })
      return
    }
    this._wifiKeyboardShown = false
    this.setData({
      wifiSelected: ssid,
      wifiSelectedTransport: 'wifi',
      wifiNeedPassword: item.secure,
      wifiPassword: '',
      wifiFocus: item.secure && this.data.guideKind !== 'wifi',
      wifiError: ''
    }, () => this.advanceWifiGuide(item.secure ? 3 : 4, true))
    if (!item.secure) {
      setTimeout(() => this.setData({ wifiFocus: false }), 200)
    }
  },

  selectCellular() {
    if (this.data.wifiConnecting || !this.data.status.cellularAvailable) return
    this._wifiKeyboardShown = false
    this.setData({
      wifiSelected: '4G',
      wifiSelectedTransport: 'cellular',
      wifiNeedPassword: false,
      wifiPassword: '',
      wifiFocus: false,
      wifiError: ''
    }, () => this.advanceWifiGuide(4, true))
  },

  onWifiPassword(e) {
    const password = e.detail.value || ''
    this.setData({ wifiPassword: password })
    if ((this._guidePending ?? this.data.guideStep) === 4) this.advanceWifiGuide(3, true)
  },

  onWifiPasswordDone(e) {
    if (!this.data.wifiModal || !this.data.wifiNeedPassword || this.data.wifiConnecting) return
    const value = e && e.detail && e.detail.value
    const password = typeof value === 'string' ? value : this.data.wifiPassword
    this._wifiKeyboardShown = false
    this.setData({ wifiPassword: password, wifiFocus: false })
    if (password.length >= 8) this.advanceWifiGuide(4)
  },

  onWifiKeyboardHeight(e) {
    if (!this.data.wifiModal || !this.data.wifiNeedPassword) return
    const height = e.detail.height
    if (height > 0) this._wifiKeyboardShown = true
    else if (height === 0 && this._wifiKeyboardShown) this.onWifiPasswordDone()
  },

  async connectWifi() {
    if (this.data.wifiConnecting || !this.client) return
    const ssid = this.data.wifiSelected
    const cellular = this.data.wifiSelectedTransport === 'cellular'
    const transport = cellular ? 'cellular' : 'wifi'
    if (!ssid) return this.setData({ wifiError: this.data.status.cellularAvailable
      ? '请选择要连接的网络' : '请选择一个 Wi-Fi' })
    if (cellular && !this.data.status.cellularAvailable) return
    if (this.data.wifiNeedPassword && this.data.wifiPassword.length < 8) {
      return this.setData({ wifiError: 'Wi-Fi 密码至少 8 位' })
    }
    this._wifiTarget = ssid
    this._wifiTargetTransport = transport
    this._wifiFirstSetup = !(this.data.status.networkConfigured ?? this.data.status.wifiConfigured)
    this.setData({ wifiConnecting: true, wifiError: '', guideHide: this.data.guideKind === 'wifi' })
    try {
      if (cellular) await this.client.request('cellular_connect', {}, 16000)
      else await this.client.request('wifi_connect', { ssid: ssid, password: this.data.wifiPassword })
      if (this._unloaded || !this.isNetworkTarget(transport, ssid)) return
      // Allow time for DHCP, credential persistence and station rollback.
      this._wifiTimer = setTimeout(async () => {
        await this.refreshStatus()
        if (this.isNetworkTarget(transport, ssid)) {
          this._wifiTarget = ''
          this.setData({ wifiConnecting: false, wifiError: '设备响应超时，请重试' })
          this.advanceWifiGuide(this.data.wifiNeedPassword ? 3 : 4, true)
        }
      }, cellular ? 38000 : 26000)
    } catch (e) {
      if (this._unloaded || !this.isNetworkTarget(transport, ssid)) return
      this._wifiTarget = ''
      this.setData({ wifiConnecting: false, wifiError: e.message || (cellular
        ? '4G 连接失败，请重试' : '连接失败，请检查密码') })
      this.advanceWifiGuide(this.data.wifiNeedPassword ? 3 : 4, true)
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

  async onRestoreVoice() {
    if (!this.client || !this.data.vmPrevious || this._restoringVoice) return
    this._restoringVoice = true
    try {
      const voices = await this.client.request('voice_restore', {})
      this.applyVoiceList(voices)
      this.toast('已切换到上一个音色')
    } catch (e) {
      this.toast(e.message || '切换音色失败')
    } finally {
      this._restoringVoice = false
    }
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
    if (this.data.audioRecordPending || this.data.audioPhonePending || this.data.audioCreating) {
      return this.toast('声音任务仍在进行，请稍候')
    }
    const mode = e.currentTarget.dataset.mode
    this.setData({
      audioMode: mode,
      audioError: '',
      audioHint: mode === 'record'
        ? '请在提示后连续清晰朗读 10–15 秒，玩具最多录 20 秒。'
        : (mode === 'phone' ? '允许使用手机麦克风后，录制 5–20 秒。'
          : '仅支持 WAV，5–20 秒；上传后会转成 16kHz 单声道。')
    })
  },

  onAudioPrimary() {
    if (this.data.audioMode === 'record') this.startRecord()
    else if (this.data.audioMode === 'phone') this.togglePhoneRecording()
    else this.createVoice()
  },

  toggleRecord() {
    if (this.data.audioMode === 'phone') this.togglePhoneRecording()
    else this.startRecord()
  },

  togglePhoneRecording() {
    if (this.data.audioRecording && this._phoneRecording) {
      this._phoneRecording.stop()
      return
    }
    if (this.data.audioPhonePending || this.data.audioRecordPending || this.data.audioCreating) return
    if (!this.client) return this.setData({ audioError: '设备未连接' })
    if (this.mock) {
      this.setData({ audioMode: 'upload', audioDone: true, audioFileName: '手机录音.wav',
        audioFile: 'mock://phone.wav', audioFileInfo: '6 秒 · 演示录音' })
      return
    }
    this.setData({ audioPhonePending: true, audioElapsed: 0, audioTime: '00:00', audioProgress: 0,
      audioError: '', audioRecordStatus: '正在启动手机麦克风…' })
    try {
      this._phoneRecording = phoneRecorder.start({
        onStart: () => {
          this.setData({ audioRecording: true, audioRecordStatus: '正在录音，至少说 5 秒' })
          this.startRecordClock()
        },
        onStop: result => {
          this.cancelPhoneRecording()
          if (!result || !result.tempFilePath || Number(result.duration) < 5000) {
            this.setData({ audioError: '录音不足 5 秒，请重新录制' })
            return
          }
          this.setData({ audioMode: 'upload', audioDone: true, audioFile: result.tempFilePath,
            audioFileName: '手机录音.wav', audioFileInfo: '录音已就绪，点击生成音色上传到玩具' })
        },
        onError: err => {
          this.cancelPhoneRecording()
          const message = (err && err.errMsg) || ''
          this.setData({ audioError: /auth|permission|denied/i.test(message)
            ? '请在小程序设置中允许使用麦克风' : (message || '手机录音失败，请重试') })
        }
      })
    } catch (err) {
      this.cancelPhoneRecording()
      this.setData({ audioError: err.message || '手机录音暂不可用' })
    }
  },

  cancelPhoneRecording() {
    if (this._phoneRecording) this._phoneRecording.cancel()
    this._phoneRecording = null
    if (!this.data.audioPhonePending) return
    this.stopRecordClock()
    this.setData({ audioPhonePending: false, audioRecording: false,
      audioRecordStatus: '点击“开始录制”，按提示连续说话' })
  },

  startRecord() {
    if (this.data.audioRecordPending || this.data.audioRecording || this.data.audioCreating) return
    const name = (this.data.audioName || '').trim().slice(0, 8)
    if (!name) return this.setData({ audioError: '请先给音色起个名字' })
    if (!this.client) return this.setData({ audioError: '设备未连接' })
    this._recordPolling = false
    this._recordPollGeneration = (this._recordPollGeneration || 0) + 1
    this._recordPhaseRank = 0
    this._recordRequest = 0
    this.setData({
      audioRecordPending: true,
      audioRecordCancellable: false,
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
        this._recordRequest = Number.isInteger(res.request) && res.request > 0 ? res.request : 0
        this.setData({ audioRecordCancellable: this._recordRequest > 0 })
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

  async cancelDeviceRecord() {
    if (!this.client || !this._recordRequest || !this.data.audioRecordPending ||
        !this.data.audioRecordCancellable || this._recordCancelPending) return
    this._recordCancelPending = true
    const generation = this._recordPollGeneration
    try {
      await this.client.request('voice_record_cancel', { request: this._recordRequest })
      if (generation === this._recordPollGeneration) this.pollDeviceRecord()
    } catch (e) {
      if (generation === this._recordPollGeneration) {
        this.setData({ audioError: e.message || '取消请求未确认，请查看录音状态' })
      }
    } finally {
      this._recordCancelPending = false
    }
  },

  pollDeviceRecord() {
    if (!this.client || !this._recordPolling || this._recordPollInFlight) return
    const generation = this._recordPollGeneration
    this._recordPollInFlight = true
    this.client.request('voice_record_status', {}).then(data => {
      if (generation === this._recordPollGeneration && this.data.audioRecordPending && data &&
          (!this._recordRequest || data.request === this._recordRequest)) {
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
      this.setData({ audioRecordStatus: this.data.audioPhonePending ? '正在结束手机录音…' : '等待玩具确认录音状态…' })
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
    this.cancelPhoneRecording()
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
        throw new Error(outcome && outcome.timeout ? '设备仍在处理或连接已断开，请重新连接查看结果' : (outcome.message || '恢复失败，请重试'))
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
