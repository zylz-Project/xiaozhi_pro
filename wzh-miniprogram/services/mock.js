const products = require('./products')

const BUILTIN = [
  { id: 'longanhuan_v3.1', name: '龙安欢', description: '活泼亲切', builtin: true },
  { id: 'longanlingxin_v3.1', name: '龙安灵心', description: '温柔自然', builtin: true },
  { id: 'longanfengyue_v3.1', name: '龙安风悦', description: '自然清晰', builtin: true },
  { id: 'xunanchuan_v3.1', name: '许南川', description: '温暖男声', builtin: true },
  { id: 'yuxiaoyun_v3.1', name: '于小云', description: '元气亲切', builtin: true },
  { id: 'wenhuaiqing_v3.1', name: '温怀清', description: '柔和故事声', builtin: true }
]

function delay(ms) {
  return new Promise(resolve => setTimeout(resolve, ms))
}

function cloneDate() {
  const d = new Date()
  const mm = d.getMonth() + 1
  const dd = d.getDate()
  return (mm < 10 ? '0' + mm : mm) + '-' + (dd < 10 ? '0' + dd : dd)
}

function productMeta(product) {
  const names = { crawlPanda: '爬爬熊猫', dinosaur: '小恐龙', redpanda: '摇尾小熊猫' }
  return {
    product: names[product.id] || product.name,
    productId: product.model,
    name: product.ap + '-' + product.hot,
    model: product.model,
    firmware: 'v1.0.8',
    fixedVoices: BUILTIN.map(v => Object.assign({}, v)),
    clones: [],
    voiceCapacity: 10
  }
}

class MockClient {
  constructor() {
    this.mock = true
    this.connected = false
    this.listeners = []
    this.deviceId = ''
    this.product = products.get('crawlPanda')
    this.device = null
    this.status = null
    this.wifiBusy = false
    this.upload = null
    this.voiceBusy = false
    this.voiceStatus = 'ok'
    this.resetClones()
  }

  resetClones() {
    this.clones = [
      { id: 'c1', name: '小满', date: '10-12' },
      { id: 'c2', name: '阿布', date: '10-13' },
      { id: 'c3', name: '小北', date: '10-14' },
      { id: 'c4', name: '桃桃', date: '10-15' },
      { id: 'c5', name: '圆圆', date: '10-16' },
      { id: 'c6', name: '阿福', date: '10-17' }
    ]
  }

  onEvent(fn) {
    if (typeof fn === 'function') this.listeners.push(fn)
    return () => {
      const i = this.listeners.indexOf(fn)
      if (i >= 0) this.listeners.splice(i, 1)
    }
  }

  emit(type, data) {
    this.listeners.slice().forEach(fn => {
      try { fn(type, data) } catch (e) {}
    })
  }

  async open() {
    await delay(80)
    return { ok: true }
  }

  async scan() {
    await delay(700)
    return [
      { deviceId: 'mock-crawl', name: 'CrawlPanda-1A2B', localName: 'CrawlPanda-1A2B', RSSI: -48, productId: 'WZH-CP01', model: 'WZH-CP01' },
      { deviceId: 'mock-dino', name: 'Dino-6C3F', localName: 'Dino-6C3F', RSSI: -62, productId: 'WZH-DN01', model: 'WZH-DN01' },
      { deviceId: 'mock-red', name: 'RedPanda-9D2E', localName: 'RedPanda-9D2E', RSSI: -71, productId: 'WZH-RP01', model: 'WZH-RP01' }
    ]
  }

  async connect(deviceId) {
    await delay(650)
    const table = {
      'mock-crawl': 'crawlPanda',
      'mock-dino': 'dinosaur',
      'mock-red': 'redpanda'
    }
    this.deviceId = deviceId || 'mock-crawl'
    this.product = products.get(table[this.deviceId] || 'crawlPanda')
    this.connected = true
    this.status = {
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
      currentVoiceId: BUILTIN[0].id,
      version: 'v1.0.8',
      voiceCount: BUILTIN.length + this.clones.length,
      voiceCapacity: 10,
      voice_management_busy: false,
      voice_management_status: 'ok',
      voice_management_cancellable: false,
      voice_management_loaded: true,
      voice_catalog: []
    }
    this.device = productMeta(this.product)
    this.device.clones = this.clones.slice()
    this.refreshVoiceStatus()
    this.emit('connected', { device: this.device })
    return { device: this.device, status: this.status }
  }

  allVoices() {
    return BUILTIN.concat(this.clones.map(c => ({ id: c.id, name: c.name, description: '', builtin: false, date: c.date })))
  }

  refreshVoiceStatus() {
    this.status.voiceCount = BUILTIN.length + this.clones.length
    this.status.voice_catalog = this.allVoices()
    this.status.voice_management_busy = this.voiceBusy
    this.status.voice_management_status = this.voiceStatus
    this.status.voice_management_cancellable = false
    this.status.voice_management_loaded = true
    this.device.clones = this.clones.slice()
  }

  currentVoice() {
    return this.allVoices().find(v => v.id === this.status.currentVoiceId || v.name === this.status.currentVoice) || BUILTIN[0]
  }

  addClone(name) {
    if (this.clones.length >= 10) return null
    const item = { id: 'c' + Date.now(), name: String(name || '我的音色').slice(0, 8), date: cloneDate() }
    this.clones.push(item)
    this.status.currentVoice = item.name
    this.status.currentVoiceId = item.id
    this.refreshVoiceStatus()
    this.emit('status', this.status)
    return item
  }

  async request(op, data) {
    await delay(90)
    if (!this.connected && op !== 'hello') {
      const err = new Error('设备未连接')
      err.code = 1004
      throw err
    }
    switch (op) {
      case 'hello':
        return { device: this.device, status: this.status, auth: { required: false } }
      case 'get_status':
        this.refreshVoiceStatus()
        return this.status
      case 'ping':
        return { pong: Date.now() }
      case 'set_volume': {
        const v = Math.max(0, Math.min(100, Number(data && data.volume)))
        this.status.volume = v
        this.emit('status', this.status)
        return { volume: v }
      }
      case 'set_voice': {
        const id = String((data && data.id) || '')
        const name = String((data && data.name) || '')
        const item = this.allVoices().find(v => (id && v.id === id) || (!id && v.name === name))
        if (!item) throw Object.assign(new Error('没有这个音色'), { code: 1003 })
        this.status.currentVoice = item.name
        this.status.currentVoiceId = item.id
        this.emit('status', this.status)
        return { id: item.id, name: item.name }
      }
      case 'audition':
        return { playing: true, name: (data && data.name) || this.currentVoice().name }
      case 'chat_start':
        this.status.state = 'speaking'
        this.emit('status', this.status)
        return { state: 'speaking' }
      case 'chat_stop':
        this.status.state = 'idle'
        this.emit('status', this.status)
        return { state: 'idle' }
      case 'voice_record': {
        const name = String((data && data.name) || '我的音色').slice(0, 8)
        const report = status => {
          this._recordStatus = { status: status, name: name }
          this.emit('voice_record', this._recordStatus)
        }
        report('starting')
        setTimeout(() => report('prompt'), 300)
        setTimeout(() => report('recording'), 900)
        setTimeout(() => {
          report('processing')
          setTimeout(() => {
            this.addClone(name)
            report('success')
          }, 900)
        }, 2500)
        return { accepted: true, name: name }
      }
      case 'voice_record_status':
        return this._recordStatus || { status: 'idle' }
      case 'wifi_scan':
        return {
          networks: [
            { ssid: 'Home', rssi: -48, band: '2.4G', secure: true, current: true, bars: 4 },
            { ssid: 'Home-Guest', rssi: -55, band: '2.4G', secure: false, current: false, bars: 3 },
            { ssid: 'CrawlPanda-1A2B', rssi: -63, band: '2.4G', secure: true, current: false, bars: 3 },
            { ssid: 'Studio-5G', rssi: -67, band: '5G', secure: true, current: false, bars: 2 }
          ]
        }
      case 'wifi_status':
        return { connected: this.status.online, ssid: this.status.ssid, ip: '192.168.1.23', rssi: this.status.rssi, band: this.status.band, signal: this.status.signal, bars: 4 }
      case 'wifi_connect': {
        const ssid = String((data && data.ssid) || '')
        const password = String((data && data.password) || '')
        if (!ssid) throw Object.assign(new Error('请选择 Wi-Fi'), { code: 1005 })
        if (ssid.indexOf('5G') >= 0) throw Object.assign(new Error('设备仅支持 2.4GHz Wi-Fi'), { code: 1005 })
        const openNetwork = ssid === 'Home-Guest'
        if (!openNetwork && password.length < 8) throw Object.assign(new Error('Wi-Fi 密码至少 8 位'), { code: 1005 })
        this.wifiBusy = true
        this.emit('wifi_state', { state: 'connecting', ssid })
        setTimeout(() => {
          this.wifiBusy = false
          this.status.ssid = ssid
          this.status.online = true
          this.status.band = '2.4G'
          this.status.signal = '强'
          this.status.rssi = -52
          this.status.latency = 22
          this.emit('wifi_state', { state: 'connected', ssid })
          this.emit('status', this.status)
        }, 1100)
        return { accepted: true, ssid }
      }
      case 'reset_factory':
        this.emit('reset', { phase: 'checking', current: 0, total: 3 })
        setTimeout(() => this.emit('reset', { phase: 'deleting_cloud', current: 1, total: 3 }), 500)
        setTimeout(() => this.emit('reset', { phase: 'clearing_wifi', current: 2, total: 3 }), 1100)
        setTimeout(() => {
          this.status.online = false
          this.status.ssid = ''
          this.clones = []
          this.refreshVoiceStatus()
          this.emit('reset', { phase: 'rebooting', current: 3, total: 3 })
        }, 1700)
        return { accepted: true }
      case 'voice_list':
        return {
          voices: this.allVoices(),
          currentVoiceId: this.status.currentVoiceId,
          currentVoice: this.status.currentVoice
        }
      case 'voice_create':
        throw Object.assign(new Error('请通过 voice_upload_begin/chunk/end 上传音频'), { code: 1003 })
      case 'voice_upload_begin': {
        const bytes = Number(data && data.bytes)
        if (!(bytes >= 160000 && bytes <= 640000) || bytes % 2) {
          throw Object.assign(new Error('音频长度必须为 5–20 秒'), { code: 1003 })
        }
        this.upload = { name: String((data && data.name) || '我的音色').slice(0, 8), bytes: bytes, received: 0 }
        return { bytes: bytes, chunkLimit: 2048 }
      }
      case 'voice_upload_chunk': {
        if (!this.upload) throw Object.assign(new Error('请先 voice_upload_begin'), { code: 1003 })
        const b64 = String((data && data.data) || '')
        const pad = b64.endsWith('==') ? 2 : (b64.endsWith('=') ? 1 : 0)
        const size = Math.floor((b64.length * 3) / 4) - pad
        this.upload.received += size
        if (this.upload.received > this.upload.bytes) throw Object.assign(new Error('上传超出长度'), { code: 1003 })
        return { received: this.upload.received, total: this.upload.bytes }
      }
      case 'voice_upload_end': {
        if (!this.upload || this.upload.received !== this.upload.bytes) {
          throw Object.assign(new Error('音频不完整'), { code: 1003 })
        }
        const name = this.upload.name
        this.upload = null
        this.voiceBusy = true
        this.voiceStatus = 'ok'
        setTimeout(() => {
          this.voiceBusy = false
          this.addClone(name)
          this.voiceStatus = 'ok'
        }, 1200)
        return { accepted: true }
      }
      case 'voice_upload_cancel':
        this.upload = null
        return { cancelled: true }
      case 'voice_rename': {
        const id = data && data.id
        const name = String((data && data.name) || '').slice(0, 8)
        const item = this.clones.find(c => c.id === id)
        if (!item || !name) throw Object.assign(new Error('重命名失败'), { code: 1003 })
        if (this.status.currentVoiceId === id) {
          this.status.currentVoice = name
        }
        item.name = name
        this.refreshVoiceStatus()
        this.emit('status', this.status)
        return { id: id, name: name }
      }
      case 'voice_delete': {
        const id = data && data.id
        const i = this.clones.findIndex(c => c.id === id)
        if (i < 0) throw Object.assign(new Error('没有这个音色'), { code: 1003 })
        const wasCurrent = this.status.currentVoiceId === id
        this.clones.splice(i, 1)
        if (wasCurrent) {
          this.status.currentVoice = BUILTIN[0].name
          this.status.currentVoiceId = BUILTIN[0].id
        }
        this.refreshVoiceStatus()
        this.emit('status', this.status)
        return { id: id }
      }
      default:
        throw Object.assign(new Error('未知操作 ' + op), { code: 1002 })
    }
  }

  close() {
    this.connected = false
    this.emit('disconnected', {})
  }
}

module.exports = MockClient
