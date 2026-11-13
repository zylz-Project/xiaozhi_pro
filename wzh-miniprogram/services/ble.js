const config = require('./config')
const products = require('./products')

const BLE = config.BLE

function delay(ms) { return new Promise(resolve => setTimeout(resolve, ms)) }

function utf8Encode(str) {
  const bytes = []
  for (let i = 0; i < str.length; i++) {
    let code = str.charCodeAt(i)
    if (code < 0x80) bytes.push(code)
    else if (code < 0x800) {
      bytes.push(0xc0 | (code >> 6), 0x80 | (code & 0x3f))
    } else if (code >= 0xd800 && code <= 0xdbff && i + 1 < str.length) {
      const next = str.charCodeAt(++i)
      const cp = 0x10000 + ((code - 0xd800) << 10) + (next - 0xdc00)
      bytes.push(0xf0 | (cp >> 18), 0x80 | ((cp >> 12) & 0x3f), 0x80 | ((cp >> 6) & 0x3f), 0x80 | (cp & 0x3f))
    } else {
      bytes.push(0xe0 | (code >> 12), 0x80 | ((code >> 6) & 0x3f), 0x80 | (code & 0x3f))
    }
  }
  return new Uint8Array(bytes).buffer
}

function utf8Decode(buffer) {
  const bytes = new Uint8Array(buffer)
  let out = ''
  for (let i = 0; i < bytes.length;) {
    const b = bytes[i++]
    if (b < 0x80) out += String.fromCharCode(b)
    else if (b < 0xe0) out += String.fromCharCode(((b & 0x1f) << 6) | (bytes[i++] & 0x3f))
    else if (b < 0xf0) out += String.fromCharCode(((b & 0x0f) << 12) | ((bytes[i++] & 0x3f) << 6) | (bytes[i++] & 0x3f))
    else {
      const cp = ((b & 0x07) << 18) | ((bytes[i++] & 0x3f) << 12) | ((bytes[i++] & 0x3f) << 6) | (bytes[i++] & 0x3f)
      out += String.fromCharCode(0xd800 + ((cp - 0x10000) >> 10), 0xdc00 + ((cp - 0x10000) & 0x3ff))
    }
  }
  return out
}

function sameUuid(a, b) { return String(a || '').toUpperCase() === String(b || '').toUpperCase() }

class BleClient {
  constructor() {
    this.mock = false
    this.listeners = []
    this.opened = false
    this.connected = false
    this.deviceId = ''
    this.serviceId = ''
    this.rxId = ''
    this.txId = ''
    this.device = null
    this.status = null
    this._id = 0
    this._pending = {}
    this._rxBytes = []
    this._devices = {}
    this._discovering = false
    this._writeQueue = Promise.resolve()
    this._mtu = 20
    this._bound = false
    this._bindEvents()
  }

  _bindEvents() {
    if (this._bound) return
    this._bound = true
    if (typeof wx.onBluetoothDeviceFound === 'function') {
      wx.onBluetoothDeviceFound(res => {
        const list = (res && res.devices) || []
        list.forEach(d => {
          if (!d || !d.deviceId) return
          const name = d.name || d.localName || ''
          if (!name) return
          const known = BLE.NAME_PREFIXES.some(p => name.indexOf(p) >= 0)
          const inServices = (d.advertisServiceUUIDs || []).some(u => sameUuid(u, BLE.SERVICE_UUID))
          if (!known && !inServices) return
          this._devices[d.deviceId] = {
            deviceId: d.deviceId,
            name: name,
            localName: d.localName || name,
            RSSI: d.RSSI || -100,
            product: products.fromName(name).id,
            model: products.fromName(name).model,
            advertisServiceUUIDs: d.advertisServiceUUIDs || []
          }
          if (this._onFound) {
            try { this._onFound(this._scanSnapshot()) } catch (e) {}
          }
        })
      })
    }
    if (typeof wx.onBLECharacteristicValueChange === 'function') {
      wx.onBLECharacteristicValueChange(res => {
        if (this.deviceId && res.deviceId && res.deviceId !== this.deviceId) return
        if (this.txId && res.characteristicId && !sameUuid(res.characteristicId, this.txId)) return
        this._onData(res.value)
      })
    }
    if (typeof wx.onBLEConnectionStateChange === 'function') {
      wx.onBLEConnectionStateChange(res => {
        if (res.deviceId === this.deviceId && !res.connected) {
          this.connected = false
          this._rejectPending(new Error('设备连接已断开'))
          this.emit('disconnected', { deviceId: res.deviceId })
        }
      })
    }
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
    if (this.opened) return { ok: true }
    await new Promise((resolve, reject) => {
      wx.openBluetoothAdapter({
        success: resolve,
        fail: err => reject(Object.assign(new Error(this._bleError(err)), { code: err.errCode }))
      })
    })
    this.opened = true
    return { ok: true }
  }

  _bleError(err) {
    const code = err && err.errCode
    if (code === 10001) return '手机蓝牙未打开，请打开蓝牙后重试'
    if (code === 10002) return '没有找到蓝牙设备'
    if (code === 10003) return '蓝牙连接失败'
    if (code === 10004) return '没有找到指定服务'
    if (code === 10005) return '没有找到指定特征值'
    if (code === 10006) return '当前连接已断开'
    if (code === 10007) return '当前特征值不支持此操作'
    if (code === 10008) return '系统蓝牙异常'
    return (err && err.errMsg) || '蓝牙操作失败'
  }

  _scanSnapshot() {
    const list = Object.keys(this._devices).map(k => this._devices[k])
    list.sort((a, b) => b.RSSI - a.RSSI)
    return list
  }

  async scan(timeout, onFound) {
    await this.open()
    this._devices = {}
    this._discovering = true
    this._onFound = typeof onFound === 'function' ? onFound : null
    try {
      await new Promise((resolve, reject) => {
        wx.startBluetoothDevicesDiscovery({
          allowDuplicatesKey: false,
          success: resolve,
          fail: err => reject(Object.assign(new Error(this._bleError(err)), { code: err.errCode }))
        })
      })
      await delay(timeout || BLE.SCAN_TIMEOUT)
    } finally {
      this._onFound = null
    }
    if (this._discovering) {
      this._discovering = false
      try { wx.stopBluetoothDevicesDiscovery({}) } catch (e) {}
    }
    return this._scanSnapshot()
  }

  async stopScan() {
    this._discovering = false
    this._onFound = null
    try { wx.stopBluetoothDevicesDiscovery({}) } catch (e) {}
  }

  async connect(deviceId, meta) {
    await this.open()
    if (this.connected) await this.close()
    this.deviceId = deviceId
    await new Promise((resolve, reject) => {
      wx.createBLEConnection({
        deviceId,
        timeout: BLE.CONNECT_TIMEOUT,
        success: resolve,
        fail: err => reject(Object.assign(new Error(this._bleError(err)), { code: err.errCode }))
      })
    })
    this.connected = true
    await this._discover()
    await this._enableNotify()
    try {
      if (typeof wx.setBLEMTU === 'function') {
        let negotiated = 0
        await new Promise(resolve => {
          wx.setBLEMTU({
            deviceId,
            mtu: 247,
            success: res => { negotiated = Number(res && res.mtu) || 0 },
            fail: () => {},
            complete: () => resolve()
          })
        })
        if (negotiated > 0) this._mtu = Math.max(20, Math.min(BLE.MTU, negotiated - 3))
      }
    } catch (e) {}
    const hello = await this.request('hello', {
      app: 'wzh-mini',
      ver: '1.0.0',
      proto: config.PROTOCOL_VERSION,
      platform: this._platform()
    }, 12000)
    const data = hello || {}
    this.device = data.device || null
    this.status = data.status || null
    if (!this.device) {
      this.device = {
        product: (meta && meta.product) || 'crawlPanda',
        productId: (meta && (meta.productId || meta.model)) || '',
        name: (meta && meta.name) || deviceId,
        model: (meta && meta.model) || '',
        firmware: 'v1.0.8',
        fixedVoices: [
          { id: 'longanhuan_v3.1', name: '龙安欢', description: '活泼亲切', builtin: true },
          { id: 'longanlingxin_v3.1', name: '龙安灵心', description: '温柔自然', builtin: true },
          { id: 'longanfengyue_v3.1', name: '龙安风悦', description: '自然清晰', builtin: true },
          { id: 'xunanchuan_v3.1', name: '许南川', description: '温暖男声', builtin: true },
          { id: 'yuxiaoyun_v3.1', name: '于小云', description: '元气亲切', builtin: true },
          { id: 'wenhuaiqing_v3.1', name: '温怀清', description: '柔和故事声', builtin: true }
        ],
        clones: [],
        voiceCapacity: 10
      }
    }
    this.emit('ready', { device: this.device, status: this.status })
    return { device: this.device, status: this.status }
  }

  async _discover() {
    const services = await new Promise((resolve, reject) => {
      wx.getBLEDeviceServices({
        deviceId: this.deviceId,
        success: res => resolve((res && res.services) || []),
        fail: err => reject(Object.assign(new Error(this._bleError(err)), { code: err.errCode }))
      })
    })
    let service = services.find(s => sameUuid(s.uuid, BLE.SERVICE_UUID))
    if (!service) service = services.find(s => s.isPrimary !== false) || services[0]
    if (!service) throw new Error('设备没有可用服务')
    this.serviceId = service.uuid
    const chars = await new Promise((resolve, reject) => {
      wx.getBLEDeviceCharacteristics({
        deviceId: this.deviceId,
        serviceId: this.serviceId,
        success: res => resolve((res && res.characteristics) || []),
        fail: err => reject(Object.assign(new Error(this._bleError(err)), { code: err.errCode }))
      })
    })
    const rx = chars.find(c => sameUuid(c.uuid, BLE.RX_UUID)) ||
      chars.find(c => c.properties && (c.properties.write || c.properties.writeNoResponse))
    const tx = chars.find(c => sameUuid(c.uuid, BLE.TX_UUID)) ||
      chars.find(c => c.properties && (c.properties.notify || c.properties.indicate))
    if (!rx || !tx) throw new Error('设备缺少读写特征值')
    this.rxId = rx.uuid
    this.txId = tx.uuid
  }

  async _enableNotify() {
    await new Promise((resolve, reject) => {
      wx.notifyBLECharacteristicValueChange({
        state: true,
        deviceId: this.deviceId,
        serviceId: this.serviceId,
        characteristicId: this.txId,
        success: resolve,
        fail: err => reject(Object.assign(new Error(this._bleError(err)), { code: err.errCode }))
      })
    })
  }

  _platform() {
    try { return (wx.getDeviceInfo && wx.getDeviceInfo().platform) || 'unknown' } catch (e) { return 'unknown' }
  }

  request(op, data, timeout) {
    if (!this.connected) return Promise.reject(Object.assign(new Error('设备未连接'), { code: 1004 }))
    const id = ++this._id
    const frame = { id, op, data: data || {} }
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        delete this._pending[id]
        reject(Object.assign(new Error('设备响应超时'), { code: 1009 }))
      }, timeout || 10000)
      this._pending[id] = { resolve, reject, timer }
      this._send(frame).catch(err => {
        clearTimeout(timer)
        delete this._pending[id]
        reject(err)
      })
    })
  }

  _send(obj) {
    const text = JSON.stringify(obj) + '\n'
    const buffer = utf8Encode(text)
    const bytes = new Uint8Array(buffer)
    const chunk = Math.max(20, this._mtu)
    const run = async () => {
      for (let offset = 0; offset < bytes.length; offset += chunk) {
        const slice = bytes.slice(offset, offset + chunk)
        await new Promise((resolve, reject) => {
          wx.writeBLECharacteristicValue({
            deviceId: this.deviceId,
            serviceId: this.serviceId,
            characteristicId: this.rxId,
            value: slice.buffer,
            success: resolve,
            fail: err => reject(Object.assign(new Error(this._bleError(err)), { code: err.errCode }))
          })
        })
      }
    }
    this._writeQueue = this._writeQueue.then(run, run)
    return this._writeQueue
  }

  _onData(buffer) {
    const bytes = new Uint8Array(buffer)
    for (let i = 0; i < bytes.length; i++) this._rxBytes.push(bytes[i])
    // 按字节寻找换行，完整行再解码：中文 UTF-8 可能被拆到两个 BLE 包里。
    let idx = this._rxBytes.indexOf(10)
    while (idx >= 0) {
      const lineBytes = new Uint8Array(this._rxBytes.slice(0, idx))
      this._rxBytes = this._rxBytes.slice(idx + 1)
      const line = utf8Decode(lineBytes.buffer).trim()
      if (line) this._onLine(line)
      idx = this._rxBytes.indexOf(10)
    }
    if (this._rxBytes.length > 8192) this._rxBytes = []
  }

  _onLine(line) {
    let msg
    try { msg = JSON.parse(line) } catch (e) { return }
    if (msg.event) {
      if (msg.event === 'status' && msg.data) this.status = msg.data
      this.emit(msg.event, msg.data || {})
      return
    }
    const id = msg.id
    if (id == null || !this._pending[id]) return
    const pending = this._pending[id]
    delete this._pending[id]
    clearTimeout(pending.timer)
    if (msg.ok === false || msg.error) {
      const err = new Error((msg.error && msg.error.message) || '设备返回错误')
      err.code = (msg.error && msg.error.code) || 1000
      pending.reject(err)
    } else {
      pending.resolve(msg.data !== undefined ? msg.data : msg)
    }
  }

  _rejectPending(err) {
    Object.keys(this._pending).forEach(id => {
      const pending = this._pending[id]
      if (!pending) return
      clearTimeout(pending.timer)
      delete this._pending[id]
      pending.reject(err)
    })
  }

  async close() {
    if (!this.deviceId) return
    const deviceId = this.deviceId
    this.connected = false
    this._rejectPending(new Error('设备连接已关闭'))
    this.deviceId = ''
    this.serviceId = ''
    this.rxId = ''
    this.txId = ''
    try {
      await new Promise(resolve => {
        wx.closeBLEConnection({ deviceId, complete: resolve })
      })
    } catch (e) {}
    this.emit('disconnected', { deviceId })
  }
}

module.exports = BleClient
