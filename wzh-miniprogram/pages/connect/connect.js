const config = require('../../services/config')
const BleClient = require('../../services/ble')
const MockClient = require('../../services/mock')
const products = require('../../services/products')
const store = require('../../services/store')
const fmt = require('../../utils/format')

// 记住上次连过的设备：下次进来自动置顶并尝试连一次。
const LAST_KEY = 'panda-last-device'
// 首次进入连接页的三步引导只自动出现一次。
const GUIDE_KEY = 'panda-connect-guide-v1'
const AUTO_RECONNECT_DELAY = 1200

const CONNECT_GUIDE = [
  {
    target: '.ble-state',
    title: '先打开蓝牙',
    text: '第一次使用要允许微信使用蓝牙。看到“蓝牙已就绪”，手机就开始找玩具了。'
  },
  {
    target: '.toy-scroll',
    title: '把它放到手边',
    text: '选一款玩具，长按开机后放在手机旁边，1 米内连接最稳。'
  },
  {
    target: '.pair-guide',
    title: '长按开机',
    text: '长按机身上的按钮约 2 秒，听到开机音后回来点“重新扫描”。'
  }
]

function platform() {
  try {
    if (wx.getDeviceInfo) return wx.getDeviceInfo().platform || 'unknown'
    return wx.getSystemInfoSync().platform || 'unknown'
  } catch (e) { return 'unknown' }
}

function isDevtools() {
  const p = platform()
  // Only the simulator gets the mock transport. PC WeChat must still try real
  // Bluetooth so a desktop session cannot silently mask a broken device link.
  return p === 'devtools'
}

function windowInfo() {
  try {
    if (wx.getWindowInfo) return wx.getWindowInfo()
    return wx.getSystemInfoSync()
  } catch (e) { return { statusBarHeight: 20 } }
}

function readLast() {
  try {
    const v = wx.getStorageSync(LAST_KEY)
    return v && v.deviceId ? v : null
  } catch (e) { return null }
}

function writeLast(item) {
  if (!item || !item.deviceId) return
  try {
    wx.setStorageSync(LAST_KEY, {
      deviceId: item.deviceId,
      name: item.name,
      product: item.product,
      ts: Date.now()
    })
  } catch (e) {}
}

function guideSeen() {
  try { return !!wx.getStorageSync(GUIDE_KEY) } catch (e) { return true }
}

function markGuideSeen() {
  try { wx.setStorageSync(GUIDE_KEY, Date.now()) } catch (e) {}
}

Page({
  data: {
    statusBarHeight: 20,
    scanning: false,
    devices: [],
    error: '',
    connecting: false,
    connectingName: '',
    isDevtools: false,
    demoAvailable: false,
    heroIcon: '/assets/crawlPanda_icon.png',
    bleLabel: '正在准备蓝牙',
    signalLevels: [1, 2, 3, 4],
    toys: ['crawlPanda', 'dinosaur', 'redpanda'].map(function (id) {
      const p = products.get(id)
      return { id: p.id, name: p.name, short: p.short, code: p.code, icon: p.icon }
    }),
    activeToy: '',
    activeName: '',
    visibleDevices: [],
    deviceCount: 0,
    emptyTitle: '还没有发现设备',
    emptySub: '确认玩具已开机、蓝牙范围内，再试一次',
    lastName: '',
    lastMissing: false,
    autoConnecting: false,
    guideOpen: false,
    guideStep: 0,
    guideDots: [0, 1, 2],
    guideCurrent: { title: '', text: '' },
    guideSpot: { left: 0, top: 0, width: 0, height: 0, radius: 18 },
    guideBubble: { place: 'below', style: 'opacity:0' }
  },

  onLoad(options) {
    const info = windowInfo()
    const picked = options && options.toy ? products.normalizeId(options.toy) : ''
    const toy = products.get(picked || store.getToy())
    this.mock = false
    this.client = null
    this.connected = false
    this.lastDevice = readLast()
    this._autoTried = false
    this._autoTimer = null
    this._guidePending = null
    this.setData({
      statusBarHeight: info.statusBarHeight || 20,
      heroIcon: toy.icon,
      isDevtools: isDevtools(),
      activeToy: picked,
      activeName: picked ? toy.name : '',
      lastName: this.lastDevice ? this.lastDevice.name : ''
    })
    this.startScan()
    if (!guideSeen()) {
      this._guideTimer = setTimeout(() => this.openGuide(), 900)
    }
  },

  onUnload() {
    if (this._autoTimer) clearTimeout(this._autoTimer)
    if (this._guideTimer) clearTimeout(this._guideTimer)
    if (this.client && !this.connected) {
      try { this.client.close() } catch (e) {}
    }
  },

  getClient() {
    if (this.client) return this.client
    if (isDevtools() && config.USE_MOCK_IN_DEVTOOLS) {
      this.client = new MockClient()
      this.mock = true
      this.setData({ bleLabel: '演示模式' })
      return this.client
    }
    this.client = new BleClient()
    this.mock = false
    return this.client
  },

  decorateDevices(list) {
    const last = this.lastDevice
    const devices = (list || []).map(d => {
      const p = products.detectProduct(d)
      const name = d.name || d.localName || '未知设备'
      return {
        deviceId: d.deviceId,
        name: name,
        product: p.id,
        icon: p.icon,
        meta: (p.model || 'WZH') + ' · ' + (d.RSSI >= -65 ? '很近' : '附近'),
        rssi: d.RSSI || -100,
        bars: fmt.signalBars(d.RSSI || -100),
        isLast: !!(last && (last.deviceId === d.deviceId || last.name === name))
      }
    })
    // 上次连过的设备置顶，其余按信号强弱。
    devices.sort((a, b) => (b.isLast ? 1 : 0) - (a.isLast ? 1 : 0) || b.rssi - a.rssi)
    return devices
  },

  renderDevices(list) {
    const all = list || this.data.devices || []
    const active = this.data.activeToy
    const name = this.data.activeName
    const visible = active ? all.filter(d => d.product === active) : all
    const lastFound = visible.some(d => d.isLast)
    this.setData({
      devices: all,
      visibleDevices: visible,
      deviceCount: visible.length,
      lastMissing: !!(this.lastDevice && !lastFound && !this.data.scanning),
      emptyTitle: active ? '还没发现' + name : '还没有发现设备',
      emptySub: active
        ? '确认' + name + '已开机、在蓝牙范围内，再点“重新扫描”'
        : '确认玩具已开机、蓝牙范围内，再试一次'
    })
  },

  // 上次连过的设备一出现就自动连一次；用户手动操作会取消。
  maybeAutoConnect(list) {
    if (this._autoTried || this.data.connecting || this.data.guideOpen) return
    const last = this.lastDevice
    if (!last) return
    const hit = (list || []).find(d => d.isLast)
    if (!hit) return
    this._autoTried = true
    this.setData({ autoConnecting: true, autoName: hit.name })
    this._autoTimer = setTimeout(() => {
      this._autoTimer = null
      if (this.data.connecting || this.data.guideOpen) {
        this.setData({ autoConnecting: false })
        return
      }
      this.setData({ autoConnecting: false })
      this.connectTo(hit, true)
    }, AUTO_RECONNECT_DELAY)
  },

  cancelAutoConnect() {
    if (this._autoTimer) {
      clearTimeout(this._autoTimer)
      this._autoTimer = null
    }
    this._autoTried = true
    if (this.data.autoConnecting) this.setData({ autoConnecting: false })
  },

  onToyFilter(e) {
    this.cancelAutoConnect()
    const id = e.currentTarget.dataset.id
    const active = this.data.activeToy === id ? '' : id
    const toy = active ? products.get(active) : null
    this.setData({ activeToy: active, activeName: toy ? toy.name : '' })
    this.renderDevices(this.data.devices)
    if (!this.data.scanning && !this.data.devices.length) this.startScan()
  },

  async startScan() {
    if (this.data.scanning || this.data.connecting) return
    const client = this.getClient()
    this.setData({ scanning: true, error: '', demoAvailable: false, bleLabel: this.mock ? '演示模式' : '正在准备蓝牙' })
    try {
      await client.open()
      if (!this.mock) this.setData({ bleLabel: '蓝牙已就绪' })
      const list = await client.scan(undefined, partial => {
        const devices = this.decorateDevices(partial)
        if (!devices.length) return
        this.setData({ scanning: true, bleLabel: this.mock ? '演示模式' : '发现 ' + devices.length + ' 台设备' })
        this.renderDevices(devices)
        this.maybeAutoConnect(devices)
      })
      const devices = this.decorateDevices(list)
      this.setData({ scanning: false, demoAvailable: false, bleLabel: this.mock ? '演示模式' : '发现 ' + devices.length + ' 台设备' })
      this.renderDevices(devices)
      this.maybeAutoConnect(devices)
    } catch (e) {
      this.setData({
        scanning: false,
        demoAvailable: isDevtools(),
        error: e.message || '蓝牙扫描失败',
        bleLabel: '蓝牙不可用'
      })
      this.renderDevices([])
    }
  },

  onScan() {
    if (this.data.connecting) return
    if (this.data.scanning) return
    this.cancelAutoConnect()
    this._autoTried = false
    this.setData({ devices: [], error: '' })
    this.startScan()
  },

  async onConnect(e) {
    if (this.data.connecting) return
    this.cancelAutoConnect()
    const id = e.currentTarget.dataset.id
    const item = this.data.devices.find(d => d.deviceId === id)
    if (!item) return
    this.connectTo(item)
  },

  async connectTo(item, auto) {
    if (!item || this.data.connecting) return
    const client = this.client || this.getClient()
    this.setData({ connecting: true, connectingName: item.name, error: '' })
    try {
      const res = await client.connect(item.deviceId, item)
      const product = products.detectProduct(res.device || item)
      store.setToy(product.id)
      writeLast({ deviceId: item.deviceId, name: item.name, product: product.id })
      const app = getApp()
      app.setBle(client, this.mock)
      app.globalData.toy = product.id
      app.globalData.lastStatus = res.status || null
      this.connected = true
      wx.redirectTo({ url: '/pages/device/device?toy=' + product.id })
    } catch (err) {
      this.setData({
        connecting: false,
        error: auto ? ('自动连接“' + item.name + '”失败，点一下设备重试') : (err.message || '连接失败，请重试')
      })
      try { await client.close() } catch (e) {}
      this.client = null
    }
  },

  async onDemo() {
    if (this.data.connecting) return
    this.client = new MockClient()
    this.mock = true
    this.setData({ isDevtools: true, demoAvailable: true, bleLabel: '演示模式' })
    try {
      const list = await this.client.scan()
      const devices = (list || []).map(d => {
        const p = products.detectProduct(d)
        return { deviceId: d.deviceId, name: d.name, product: p.id, icon: p.icon, meta: p.model + ' · 演示', rssi: d.RSSI, bars: fmt.signalBars(d.RSSI) }
      })
      this.setData({ scanning: false })
      this.renderDevices(devices)
      if (devices[0]) await this.connectTo(devices[0])
    } catch (e) {
      this.setData({ error: e.message || '演示设备启动失败' })
    }
  },

  // ---------- 首次连接引导（与详情页同一套聚光灯/气泡皮肤） ----------

  openGuide() {
    this.cancelAutoConnect()
    this._guidePending = null
    this.setData({
      guideOpen: true,
      guideStep: 0,
      guideCurrent: { title: CONNECT_GUIDE[0].title, text: CONNECT_GUIDE[0].text },
      guideSpot: { left: 0, top: 0, width: 0, height: 0, radius: 18 },
      guideBubble: { place: 'below', style: 'opacity:0' }
    }, () => setTimeout(() => this.showGuideStep(0), 120))
  },

  showGuideStep(index) {
    const step = CONNECT_GUIDE[index]
    if (!step) return this.closeGuide()
    if (this._guidePending !== undefined && this._guidePending !== null) return
    this._guidePending = index
    setTimeout(() => this.measureGuide(index), 140)
  },

  measureGuide(index) {
    const step = CONNECT_GUIDE[index]
    if (!this.data.guideOpen || !step) { this._guidePending = null; return }
    const query = wx.createSelectorQuery()
    query.select('.connect-page').boundingClientRect()
    query.select(step.target).boundingClientRect()
    query.exec((res) => {
      if (!this.data.guideOpen) { this._guidePending = null; return }
      const page = res && res[0]
      const rect = res && res[1]
      const vw = (page && page.width) || 390
      const vh = (page && page.height) || 760
      const spot = rect ? {
        left: Math.max(0, rect.left - 6),
        top: Math.max(0, rect.top - 6),
        width: Math.min(vw, rect.width + 12),
        height: Math.min(vh, rect.height + 12),
        radius: 18
      } : { left: vw / 2 - 60, top: vh / 2 - 40, width: 120, height: 80, radius: 18 }
      const bw = Math.min(300, vw - 28)
      const below = spot.top + spot.height / 2 < vh * 0.56
      const left = Math.max(14, Math.min(spot.left + spot.width / 2 - bw / 2, vw - bw - 14))
      const style = below
        ? 'left:' + left + 'px;top:' + (spot.top + spot.height + 14) + 'px;width:' + bw + 'px'
        : 'left:' + left + 'px;bottom:' + Math.max(14, vh - spot.top + 14) + 'px;width:' + bw + 'px'
      this._guidePending = null
      this.setData({
        guideStep: index,
        guideCurrent: { title: step.title, text: step.text },
        guideSpot: { left: spot.left, top: spot.top, width: spot.width, height: spot.height, radius: spot.radius },
        guideBubble: { place: below ? 'below' : 'above', style: style }
      })
    })
  },

  nextGuide() {
    if (this._guidePending !== undefined && this._guidePending !== null) return
    if (this.data.guideStep + 1 >= CONNECT_GUIDE.length) return this.closeGuide()
    this.showGuideStep(this.data.guideStep + 1)
  },

  closeGuide() {
    this._guidePending = null
    markGuideSeen()
    this.setData({ guideOpen: false })
  },

  noop() {}
})
