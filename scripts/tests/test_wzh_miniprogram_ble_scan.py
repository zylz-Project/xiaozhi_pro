import subprocess
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


class MiniProgramBleScanTest(unittest.TestCase):
    def run_js(self, script):
        subprocess.run(['node', '-e', script], cwd=ROOT, check=True, timeout=10)

    def test_pending_write_fragments_do_not_cross_a_reconnection(self):
        self.run_js(r'''
const assert = require('node:assert/strict')
const BleClient = require('./wzh-miniprogram/services/ble')
const turn = () => new Promise(resolve => setImmediate(resolve))
const writes = []
let release
global.wx = {
  writeBLECharacteristicValue: ({ deviceId, success }) => { writes.push(deviceId); release = success },
  closeBLEConnection: ({ complete }) => complete()
}
;(async () => {
  const client = new BleClient()
  Object.assign(client, { connected: true, deviceId: 'old', serviceId: 'service', rxId: 'rx' })
  const first = assert.rejects(client.request('test', { payload: 'x'.repeat(80) }))
  const queued = assert.rejects(client.request('test', {}))
  await turn()
  assert.deepEqual(writes, ['old'])
  await client.close()
  await Promise.all([first, queued])
  Object.assign(client, { connected: true, deviceId: 'replacement' })
  release()
  await turn()
  assert.deepEqual(writes, ['old'])
})().catch(error => { console.error(error); process.exitCode = 1 })
''')

    def test_recent_device_metadata_does_not_trigger_connect(self):
        self.run_js(r'''
const assert = require('node:assert/strict')
let page, scans = 0, attempts = 0
global.Page = value => { page = value }
global.getApp = () => ({ globalData: {} })
global.wx = {
  getDeviceInfo: () => ({ platform: 'ios' }),
  getStorageSync: key => key === 'panda-last-device' ? {
    deviceId: 'red', product: 'redpanda', name: 'RedPanda-0001', identity: 'instance', ts: Date.now()
  } : true
}
require('./wzh-miniprogram/pages/connect/connect.js')
page.setData = (data, done) => { Object.assign(page.data, data); if (done) done() }
const client = {
  connect: async () => { attempts++ },
  stopScan: async () => {}, close: async () => {},
  scan: async () => { scans++; return [] }
}
page.getClient = () => { page.client = client; return client }
page.onLoad({})
page.onShow()
setImmediate(() => {
  assert.equal(attempts, 0)
  assert.equal(scans, 1)
  assert.equal(page.data.connecting, false)
  assert.equal(page.data.error, '')
  assert.equal(page.data.lastName, 'RedPanda-0001')
  page.onUnload()
})
''')

    def test_live_reports_supplement_cached_names_without_duplicates(self):
        self.run_js(r'''
const assert = require('node:assert/strict')
const BleClient = require('./wzh-miniprogram/services/ble')
const { BLE } = require('./wzh-miniprogram/services/config')
const turn = () => new Promise(resolve => setImmediate(resolve))
let report, feedSnapshot
global.wx = {
  getDeviceInfo: () => ({ platform: 'android' }),
  onBluetoothDeviceFound: fn => { report = devices => fn({ devices }) },
  openBluetoothAdapter: ({ success }) => success(),
  startBluetoothDevicesDiscovery: options => {
    assert.equal(options.powerLevel, 'high')
    assert.equal(options.allowDuplicatesKey, true)
    assert.equal(options.interval, 0)
    assert.equal(options.services, undefined)
    options.success()
  },
  getBluetoothDevices: ({ success }) => { feedSnapshot = devices => success({ devices }) },
  stopBluetoothDevicesDiscovery: ({ complete }) => complete()
}
;(async () => {
  const client = new BleClient()
  const updates = []
  const result = client.scan(5000, devices => updates.push(devices))
  await turn()
  feedSnapshot([{ deviceId: 'red', name: 'RedPanda-0001', RSSI: -70 }])
  assert.equal(updates.at(-1)[0].live, false)

  report([
    { deviceId: 'red', name: 'Generic Peripheral', localName: 'redpanda-0001', RSSI: -55 },
    { deviceId: 'crawl', localName: 'CrawlPanda-0002', RSSI: -64 },
    { deviceId: 'dino', name: 'Dino-0003', RSSI: -67 },
    { deviceId: 'unnamed', advertisServiceUUIDs: [BLE.SERVICE_UUID], RSSI: -83 },
    { deviceId: 'other', name: 'Headphones', RSSI: -20 }
  ])
  const devices = updates.at(-1)
  assert.deepEqual(devices.map(d => d.product), ['redpanda', 'crawlPanda', 'dinosaur', ''])
  assert.equal(devices[0].name, 'redpanda-0001')
  assert.equal(devices[0].live, true)

  report([{ deviceId: 'red', RSSI: -56 }])
  assert.equal(updates.length, 2)
  report([{ deviceId: 'red', name: 'Dino-9999', RSSI: -56 }])
  assert.equal(updates.length, 2)
  feedSnapshot([{ deviceId: 'red', localName: 'Dino-9999', RSSI: -90 }])
  assert.equal(updates.length, 2)
  await client.stopScan()
  assert.equal((await result)[0].name, 'redpanda-0001')
  report([{ deviceId: 'late', localName: 'RedPanda-AAAA' }])
  assert.equal(updates.length, 2)
})().catch(error => { console.error(error); process.exitCode = 1 })
''')

    def test_failed_start_and_pending_stop_can_be_retried(self):
        self.run_js(r'''
const assert = require('node:assert/strict')
const BleClient = require('./wzh-miniprogram/services/ble')
const turn = () => new Promise(resolve => setImmediate(resolve))
let auth = 'denied', starts = 0, binds = 0, finishStop
global.wx = {
  getDeviceInfo: () => ({ platform: 'ios' }),
  getAppAuthorizeSetting: () => ({ bluetoothAuthorized: auth }),
  onBluetoothDeviceFound: () => { binds++ },
  openBluetoothAdapter: ({ success }) => success(),
  startBluetoothDevicesDiscovery: options => {
    assert.equal(options.powerLevel, undefined)
    starts++
    if (auth === 'denied') options.fail({ errCode: 10001 })
    else options.success()
  },
  stopBluetoothDevicesDiscovery: ({ complete }) => { finishStop = complete }
}
;(async () => {
  const client = new BleClient()
  const failure = assert.rejects(client.scan(5000), /微信的蓝牙权限/)
  await turn()
  finishStop()
  await failure

  auth = 'authorized'
  client.scan(5000)
  await turn()
  const stop = client.stopScan()
  await turn()
  client.scan(5000)
  assert.equal(starts, 2)
  finishStop()
  await stop

  const replacement = new BleClient()
  replacement.scan(5000)
  await turn()
  assert.equal(starts, 3)
  assert.equal(binds, 1)
  const close = replacement.close()
  await turn()
  finishStop()
  await close
})().catch(error => { console.error(error); process.exitCode = 1 })
''')

    def test_empty_discovery_and_disabled_adapter_have_distinct_outcomes(self):
        self.run_js(r'''
const assert = require('node:assert/strict')
const BleClient = require('./wzh-miniprogram/services/ble')
let available = true, discovering = false, denied = false, starts = 0
let onDeviceFound
global.wx = {
  getDeviceInfo: () => ({ platform: 'android' }),
  getAppAuthorizeSetting: () => ({ bluetoothAuthorized: denied ? 'denied' : 'authorized' }),
  onBluetoothDeviceFound: cb => { onDeviceFound = cb },
  openBluetoothAdapter: ({ success }) => success(),
  getBluetoothAdapterState: ({ success }) => success({ available, discovering }),
  startBluetoothDevicesDiscovery: ({ fail }) => {
    starts++
    fail({ errCode: 10008 })
  },
  stopBluetoothDevicesDiscovery: ({ complete }) => complete()
}
;(async () => {
  const client = new BleClient()
  assert.deepEqual(await client.scan(5), [])
  assert.equal(starts, 2)

  discovering = true
  const live = client.scan(30)
  await new Promise(resolve => setImmediate(resolve))
  onDeviceFound({ devices: [{ deviceId: 'red', localName: 'RedPanda-0123', RSSI: -55 }] })
  assert.equal((await live)[0].deviceId, 'red')
  assert.equal(starts, 3)

  available = false
  await assert.rejects(client.scan(5), /蓝牙未打开/)
  available = true; denied = true
  await assert.rejects(client.scan(5), /蓝牙权限已关闭/)
})().catch(error => { console.error(error); process.exitCode = 1 })
''')

    def test_page_restarts_scan_and_ignores_obsolete_results(self):
        self.run_js(r'''
const assert = require('node:assert/strict')
const turn = () => new Promise(resolve => setImmediate(resolve))
let page
const connectionAttempts = []
global.Page = definition => { page = definition }
global.wx = {
  getDeviceInfo: () => ({ platform: 'ios' }),
  getStorageSync: key => key === 'panda-last-device' ? { deviceId: 'red', product: 'redpanda' } : 1
}
require('./wzh-miniprogram/pages/connect/connect.js')
page.setData = (data, done) => { Object.assign(page.data, data); if (done) done() }
const rounds = []
const client = {
  stopScan: async () => {},
  scan: (timeout, found) => new Promise(resolve => rounds.push({ found, resolve })),
  close: async () => {}
}
page.getClient = () => { page.client = client; return client }
const red = { deviceId: 'red', product: 'redpanda', name: 'RedPanda-0001', RSSI: -55 }
const crawl = { deviceId: 'crawl', product: 'crawlPanda', name: 'CrawlPanda-0002', RSSI: -55 }
page.connectTo = item => { connectionAttempts.push(item.deviceId) }
;(async () => {
  page.onLoad({})
  page.onShow()
  await turn()
  rounds[0].found([{ ...red, live: false }])
  assert.equal(page.data.visibleDevices.length, 1)
  assert.equal(connectionAttempts.length, 0)

  page.onScan()
  await turn()
  assert.equal(rounds.length, 2)
  rounds[0].resolve([red])
  await turn()
  assert.equal(page.data.visibleDevices.length, 0)
  rounds[1].found([{ ...red, live: true }, crawl])
  assert.equal(page.data.visibleDevices.length, 2)
  assert.equal(connectionAttempts.length, 0)

  page.onHide()
  rounds[1].resolve([])
  page.onShow()
  await turn()
  rounds[2].resolve([red])
  await turn()
  assert.equal(page.data.visibleDevices.length, 1)
  assert.equal(page.data.scanning, false)
  assert.equal(connectionAttempts.length, 0)
  await page.onConnect({ currentTarget: { dataset: { id: 'red' } } })
  assert.deepEqual(connectionAttempts, ['red'])
  page.onUnload()
})().catch(error => { console.error(error); process.exitCode = 1 })
''')
