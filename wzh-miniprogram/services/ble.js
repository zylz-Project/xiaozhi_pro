const config = require('./config')
const products = require('./products')

const BLE = config.BLE

function delay(ms) { return new Promise(resolve => setTimeout(resolve, ms)) }

async function withTimeout(operation, ms) {
  let timer
  try {
    return await Promise.race([operation, new Promise((_, reject) => {
      timer = setTimeout(() => reject(new Error('蓝牙响应超时')), Math.max(1, ms))
    })])
  } finally {
    clearTimeout(timer)
  }
}

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

// 微信的蓝牙事件是全局的，详情页沿用连接页交接的客户端。
let eventClient = null
let eventsBound = false

function broadcastPrefix(name) {
  return BLE.NAME_PREFIXES.find(prefix => name.toLowerCase().startsWith(prefix.toLowerCase()))
}

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
    this._scan = null
    this._writeQueue = Promise.resolve()
    this._connectionGeneration = 0
    this._mtu = 20
  }

  _bindEvents() {
    eventClient = this
    if (eventsBound) return
    eventsBound = true
    if (typeof wx.onBluetoothDeviceFound === 'function') {
      wx.onBluetoothDeviceFound(res => {
        const client = eventClient
        if (client && client._scan && client._scan.receiving) client._mergeScanDevices(client._scan, res && res.devices, true)
      })
    }
    if (typeof wx.onBluetoothAdapterStateChange === 'function') {
      wx.onBluetoothAdapterStateChange(res => {
        const client = eventClient
        if (!client || res.available !== false) return
        client.opened = false
        if (client._scan && !client._scan.cancelled) {
          client._scan.error = client._bluetoothError({ errCode: 10001 })
          client._scan.finish()
        }
      })
    }
    if (typeof wx.onBLECharacteristicValueChange === 'function') {
      wx.onBLECharacteristicValueChange(res => {
        const client = eventClient
        if (!client || !client.deviceId || (res.deviceId && res.deviceId !== client.deviceId)) return
        if (client.txId && res.characteristicId && !sameUuid(res.characteristicId, client.txId)) return
        client._onData(res.value)
      })
    }
    if (typeof wx.onBLEConnectionStateChange === 'function') {
      wx.onBLEConnectionStateChange(res => {
        const client = eventClient
        if (client && res.deviceId === client.deviceId && !res.connected) {
          client.connected = false
          ++client._connectionGeneration
          client._rxBytes = []
          client._rejectPending(new Error('设备连接已断开'))
          client.emit('disconnected', { deviceId: res.deviceId })
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
    this._bindEvents()
    if (this.opened) return { ok: true }
    await new Promise((resolve, reject) => {
      wx.openBluetoothAdapter({
        success: resolve,
        fail: err => reject(this._bluetoothError(err))
      })
    })
    this.opened = true
    return { ok: true }
  }

  _bluetoothError(err) {
    let auth
    try { auth = wx.getAppAuthorizeSetting && wx.getAppAuthorizeSetting() } catch (e) {}
    const message = auth && auth.bluetoothAuthorized === 'denied'
      ? '微信的蓝牙权限已关闭，请在手机设置中允许后重试'
      : this._bleError(err)
    return Object.assign(new Error(message), {
      code: err.errCode,
      permissionDenied: Boolean(auth && auth.bluetoothAuthorized === 'denied')
    })
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

  _scanSnapshot(scan) {
    return Array.from(scan.devices.values()).sort((a, b) => Number(b.live) - Number(a.live) || b.RSSI - a.RSSI)
  }

  _mergeScanDevices(scan, list, live) {
    if (this._scan !== scan || scan.cancelled || scan.error || !Array.isArray(list)) return
    if (live && list.length && scan.firstReportMs === undefined) scan.firstReportMs = Date.now() - scan.startedAt
    let changed = false
    for (const d of list) {
      if (!d || !d.deviceId) continue
      if (live) scan.seen.add(d.deviceId)
      const previous = scan.devices.get(d.deviceId)
      const localName = String(d.localName || '').trim()
      const systemName = String(d.name || '').trim()
      const names = [localName, systemName].filter(broadcastPrefix)
      const productName = names.find(products.matchName)
      const reportedName = productName || names[0]
      // 型号明确的广播名优先，其次是系统名，最后才是通用名称。
      const nameRank = productName ? (productName === localName ? 3 : 2) : Number(Boolean(reportedName))
      const keepName = previous && previous.knownName &&
        (previous.nameRank > nameRank || (previous.live && !live))
      const knownName = keepName ? previous.knownName : reportedName
      const services = d.advertisServiceUUIDs || []
      if (!knownName && !previous && !services.some(u => sameUuid(u, BLE.SERVICE_UUID))) continue
      const product = knownName && products.matchName(knownName)
      const name = knownName || (previous && previous.name) || localName || systemName || '待识别蓝牙设备'
      const device = {
        deviceId: d.deviceId,
        name,
        knownName,
        nameRank: keepName ? previous.nameRank : nameRank,
        product: product ? product.id : '',
        model: product ? product.model : '',
        RSSI: (previous && previous.live && !live) || !Number.isFinite(d.RSSI)
          ? (previous ? previous.RSSI : -100) : d.RSSI,
        live: scan.seen.has(d.deviceId)
      }
      scan.devices.set(d.deviceId, device)
      // 高频广播可补全晚到的字段，RSSI 的小幅波动留到扫描结束时刷新。
      changed = changed || !previous || previous.name !== device.name || previous.live !== device.live
    }
    if (changed && scan.firstMatchMs === undefined) scan.firstMatchMs = Date.now() - scan.startedAt
    if (changed && typeof scan.onFound === 'function') scan.onFound(this._scanSnapshot(scan))
  }

  _readScanSnapshot(scan) {
    if (scan.cancelled || scan.error || typeof wx.getBluetoothDevices !== 'function') return
    wx.getBluetoothDevices({ success: res => this._mergeScanDevices(scan, res && res.devices, false) })
  }

  scan(timeout, onFound) {
    if (this._scan) return this._scan.done
    const scan = { devices: new Map(), seen: new Set(), onFound, startedAt: Date.now(), cancelled: false }
    const finished = new Promise(resolve => { scan.finish = resolve })
    this._scan = scan
    scan.done = this._runScan(scan, finished, timeout || BLE.SCAN_TIMEOUT)
    return scan.done
  }

  _startDiscovery(options) {
    return new Promise((resolve, reject) => wx.startBluetoothDevicesDiscovery({
      ...options,
      allowDuplicatesKey: true,
      interval: 0,
      success: resolve,
      fail: err => reject(this._bluetoothError(err))
    }))
  }

  async _readAdapterState() {
    if (typeof wx.getBluetoothAdapterState !== 'function') return null
    return new Promise(resolve => wx.getBluetoothAdapterState({
      success: resolve,
      fail: () => resolve(null)
    }))
  }

  async _runScan(scan, finished, timeout) {
    let deadline, snapshotTimer, startRequested
    try {
      try {
        await this.open()
      } catch (err) {
        if (err.permissionDenied) throw err
        if (err.code === 10002) return this._scanSnapshot(scan)
        if (err.code !== 10008) throw err
        const state = await this._readAdapterState()
        if (state?.available === false) throw this._bluetoothError({ errCode: 10001 })
        if (!state?.available) throw err
        this.opened = true
      }
      if (scan.cancelled) return []
      if (scan.error) throw scan.error
      scan.adapterMs = Date.now() - scan.startedAt
      startRequested = true
      scan.receiving = true
      const power = this._platform() === 'android' ? { powerLevel: 'high' } : {}
      try {
        await this._startDiscovery(power)
      } catch (err) {
        if (err.permissionDenied) throw err
        if (err.code === 10002) return this._scanSnapshot(scan)
        if (err.code !== 10008) throw err
        // The adapter may already be discovering after a foreground/quick
        // reconnect transition. Keep listening to its live device reports.
        const state = await this._readAdapterState()
        if (scan.cancelled) return this._scanSnapshot(scan)
        if (scan.error) throw scan.error
        if (state?.available === false) throw this._bluetoothError({ errCode: 10001 })
        if (!state?.available) throw err
        if (!state.discovering) {
          try {
            await this._startDiscovery({})
          } catch (retryError) {
            if (retryError.permissionDenied || ![10002, 10008].includes(retryError.code)) throw retryError
            const afterRetry = await this._readAdapterState()
            if (scan.error) throw scan.error
            if (afterRetry?.available === false) throw this._bluetoothError({ errCode: 10001 })
            if (!afterRetry?.available) throw retryError
            if (!afterRetry.discovering) return this._scanSnapshot(scan)
          }
        }
      }
      if (scan.cancelled) return this._scanSnapshot(scan)
      scan.discoveryMs = Date.now() - scan.startedAt
      this._readScanSnapshot(scan)
      snapshotTimer = setTimeout(() => this._readScanSnapshot(scan), 1000)
      deadline = setTimeout(scan.finish, timeout)
      await finished
      if (scan.error) throw scan.error
      return this._scanSnapshot(scan)
    } catch (err) {
      scan.error = err
      throw err
    } finally {
      scan.cancelled = true
      clearTimeout(snapshotTimer)
      clearTimeout(deadline)
      if (startRequested && typeof wx.stopBluetoothDevicesDiscovery === 'function') {
        try { await new Promise(resolve => wx.stopBluetoothDevicesDiscovery({ complete: resolve })) } catch (e) {}
      }
      this._scan = null
      console.info('[WZH BLE] scan', {
        adapterMs: scan.adapterMs,
        discoveryMs: scan.discoveryMs,
        firstReportMs: scan.firstReportMs,
        firstMatchMs: scan.firstMatchMs,
        reported: scan.seen.size,
        matched: scan.devices.size,
        code: scan.error && scan.error.code
      })
    }
  }

  async stopScan() {
    const scan = this._scan
    if (!scan) return
    scan.cancelled = true
    scan.finish()
    try { await scan.done } catch (e) {}
  }

  async resume(identity) {
    if (!this.connected || !this.device || this.device.identity !== identity) throw new Error('设备需要重新连接')
    this.status = await this.request('get_status', {}, 900)
    return { device: this.device, status: this.status }
  }

  async connect(deviceId, meta, quickTimeout) {
    const startedAt = Date.now()
    const timings = {}
    try {
      return await this._connect(deviceId, meta, quickTimeout, startedAt, timings)
    } finally {
      console.info('[WZH BLE] connect', { quick: Boolean(quickTimeout), ...timings, totalMs: Date.now() - startedAt })
    }
  }

  async _connect(deviceId, meta, quickTimeout, startedAt, timings) {
    const wait = operation => quickTimeout ? withTimeout(operation, quickTimeout - (Date.now() - startedAt)) : operation
    await this.stopScan()
    if (this.connected) await this.close()
    await wait(this.open())
    const unidentified = meta && !meta.product
    this.deviceId = deviceId
    this._mtu = 20
    this._writeQueue = Promise.resolve()
    ++this._connectionGeneration
    await new Promise((resolve, reject) => {
      wx.createBLEConnection({
        deviceId,
        timeout: quickTimeout ? Math.max(1, quickTimeout - (Date.now() - startedAt)) : BLE.CONNECT_TIMEOUT,
        success: resolve,
        fail: err => reject(Object.assign(new Error(this._bleError(err)), { code: err.errCode }))
      })
    })
    this.connected = true
    timings.linkMs = Date.now() - startedAt
    await this._discover(unidentified, wait)
    timings.gattMs = Date.now() - startedAt
    await wait(this._enableNotify())
    timings.notifyMs = Date.now() - startedAt
    try {
      if (typeof wx.setBLEMTU === 'function') {
        let negotiated = 0
        await withTimeout(new Promise(resolve => {
          wx.setBLEMTU({
            deviceId,
            mtu: 247,
            success: res => { negotiated = Number(res && res.mtu) || 0 },
            fail: () => {},
            complete: () => resolve()
          })
        }), quickTimeout ? 150 : 450)
        if (negotiated > 0) this._mtu = Math.max(20, Math.min(BLE.MTU, negotiated - 3))
      }
    } catch (e) {}
    const hello = await wait(this.request('hello', {
      app: 'wzh-mini',
      ver: '1.0.0',
      proto: config.PROTOCOL_VERSION,
      platform: this._platform()
    }, 12000))
    timings.helloMs = Date.now() - startedAt
    const data = hello || {}
    if (unidentified && !data.device) throw new Error('这台蓝牙设备未返回玩具信息')
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

  async _discover(requireNus, wait = operation => operation) {
    const services = await wait(new Promise((resolve, reject) => {
      wx.getBLEDeviceServices({
        deviceId: this.deviceId,
        success: res => resolve((res && res.services) || []),
        fail: err => reject(Object.assign(new Error(this._bleError(err)), { code: err.errCode }))
      })
    }))
    let service = services.find(s => sameUuid(s.uuid, BLE.SERVICE_UUID))
    if (!service && !requireNus) service = services.find(s => s.isPrimary !== false) || services[0]
    if (!service) throw new Error('设备没有可用服务')
    this.serviceId = service.uuid
    const chars = await wait(new Promise((resolve, reject) => {
      wx.getBLEDeviceCharacteristics({
        deviceId: this.deviceId,
        serviceId: this.serviceId,
        success: res => resolve((res && res.characteristics) || []),
        fail: err => reject(Object.assign(new Error(this._bleError(err)), { code: err.errCode }))
      })
    }))
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
    const generation = this._connectionGeneration
    const { deviceId, serviceId, rxId } = this
    const run = async () => {
      for (let offset = 0; offset < bytes.length; offset += chunk) {
        if (!this.connected || generation !== this._connectionGeneration || !this._pending[obj.id]) {
          throw new Error('蓝牙请求已结束，请重试')
        }
        const slice = bytes.slice(offset, offset + chunk)
        await new Promise((resolve, reject) => {
          wx.writeBLECharacteristicValue({
            deviceId,
            serviceId,
            characteristicId: rxId,
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
    await this.stopScan()
    if (eventClient === this) eventClient = null
    this.opened = false
    if (!this.deviceId) return
    const deviceId = this.deviceId
    this.connected = false
    ++this._connectionGeneration
    this._rxBytes = []
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
