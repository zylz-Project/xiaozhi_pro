import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class MiniProgramWifiTest(unittest.TestCase):
    def test_first_setup_then_actual_chat_and_reset_epoch(self):
        script = r'''
const assert = require('node:assert/strict')
let page
const timers = new Map(), app = { globalData: {} }
let timer = 0
global.setTimeout = fn => { timers.set(++timer, fn); return timer }
global.clearTimeout = id => timers.delete(id)
const flush = () => {
  for (let limit = 0; timers.size; limit++) {
    assert.ok(limit < 20, 'guide timers repeatedly rearmed')
    const [id, fn] = timers.entries().next().value
    timers.delete(id)
    fn()
  }
}
global.Page = value => { page = value }
global.getApp = () => app
global.wx = {
  createSelectorQuery: () => ({
    select: () => ({ boundingClientRect() {} }),
    exec: fn => fn([{ width: 390, height: 750 }, { left: 20, top: 80, width: 100, height: 30 }])
  })
}
require('./wzh-miniprogram/pages/device/device.js')
page.setData = (data, done) => { Object.assign(page.data, data); if (done) done() }
page.syncRailDrift = page.measureConsoleView = page.toast = page.refreshStatus = () => {}
page._guideReady = true
const device = { identity: 'one', guide_epoch: 7, model: 'WZH-RP01' }
const ops = []
let scanDone
page.client = { device, request: async (op, data) => {
  ops.push([op, data])
  if (op === 'wifi_scan') return new Promise(resolve => { scanDone = resolve })
  if (op === 'guide_complete') assert.equal(data.epoch, device.guide_epoch)
} }
const offline = { online: false, state: 'idle', guide_done: false, wifi: { configured: false }, onboarding: { done: false } }
;(async () => {
  page.syncDevice(device)
  page.applyStatus(offline)
  flush()
  assert.deepEqual([page.data.guideKind, page.data.guideStep], ['wifi', 0])
  page.toggleConsole(); flush(); assert.equal(page.data.guideStep, 1)
  page.openWifi(); flush()
  assert.deepEqual([page.data.wifiScanning, page.data.guideHide, page.data.guideStep], [true, true, 1])
  scanDone({ networks: [{ ssid: 'test', band: '2.4G', secure: true }] })
  await new Promise(resolve => setImmediate(resolve))
  flush()
  assert.deepEqual([page.data.wifiScanning, page.data.guideHide, page.data.guideStep], [false, false, 2])
  page.selectWifi({ currentTarget: { dataset: { ssid: 'test' } } }); flush(); assert.equal(page.data.guideStep, 3)
  page.onWifiPassword({ detail: { value: '1234567' } })
  page.onWifiPasswordDone(); flush(); assert.equal(page.data.guideStep, 3)
  page.onWifiKeyboardHeight({ detail: { height: 240 } })
  page.onWifiPassword({ detail: { value: '12345678' } }); flush(); assert.equal(page.data.guideStep, 3)
  page.onWifiPassword({ detail: { value: '123456789' } }); flush(); assert.equal(page.data.guideStep, 3)
  page.onWifiKeyboardHeight({ detail: { height: 0 } }); flush(); assert.equal(page.data.guideStep, 4)
  await page.connectWifi()
  page.onBleEvent('wifi_state', { state: 'failed', ssid: 'test' }); flush(); assert.equal(page.data.guideStep, 3)
  page.onWifiPassword({ detail: { value: '12345679' } }); flush(); assert.equal(page.data.guideStep, 3)
  page.onWifiPasswordDone({ detail: { value: '123456790' } }); flush(); assert.equal(page.data.guideStep, 4)
  await page.connectWifi()
  page.onBleEvent('wifi_state', { state: 'connected', ssid: 'other' })
  assert.equal(page.data.wifiConnecting, true)
  page.onBleEvent('wifi_state', { state: 'connected', ssid: 'test' }); flush()
  assert.deepEqual([page.data.wifiModal, page.data.expanded, page.data.guideKind, page.data.guideStep],
    [false, false, 'first', 0])
  page.nextGuide(); flush(); page.nextGuide(); flush()
  page.nextGuide(); flush(); assert.equal(page.data.guideStep, 2)
  page.onPrimary(); await Promise.resolve(); flush()
  assert.equal(page.data.guideStep, 3)
  for (let i = 3; i < 15; i++) { page.nextGuide(); await Promise.resolve(); flush() }
  assert.deepEqual(ops.map(([op]) => op), ['wifi_scan', 'wifi_connect', 'wifi_connect', 'chat_start', 'guide_complete'])
  assert.deepEqual(ops.slice(1, 3).map(([, data]) => data.password), ['123456789', '123456790'])
  assert.equal(page.data.guideOpen, false)
  assert.equal(page._guideDone, true)
  page._guidePaused = false
  page.scheduleFirstGuide(); flush(); assert.equal(page.data.guideOpen, false)
  page.openGuide(); flush(); assert.equal(page.data.guideKind, 'replay')
  await page.closeGuide()
  assert.equal(ops.length, 5)
  page.client.device = { ...device, guide_epoch: 8 }
  page.syncDevice(page.client.device)
  page.applyStatus(offline); flush()
  assert.deepEqual([page.data.guideKind, page.data.guideStep], ['wifi', 0])
})().catch(error => { console.error(error); process.exitCode = 1 })
'''
        subprocess.run(['node', '-e', script], cwd=ROOT, check=True, timeout=10)

    def test_real_scan_payload_preserves_networks_and_prefers_supported_band(self):
        script = r'''
const assert = require('assert')
let page
const app = {globalData: {}}
global.getApp = () => app
global.Page = p => { page = p }
global.wx = {}
require('./wzh-miniprogram/pages/device/device.js')
const p = Object.assign({}, page)
p.data = JSON.parse(JSON.stringify(page.data))
p.setData = values => Object.assign(p.data, values)
let timeout
p.client = {request: async (op, data, ms) => {
  assert.strictEqual(op, 'wifi_scan'); timeout = ms
  return {networks: [
    {ssid: 'Shared', band: '5G', rssi: -35, secure: true},
    {ssid: 'Shared', band: '2.4G', rssi: -70, secure: true},
    {ssid: 'Five', band: '5G', rssi: -40, secure: true},
    {ssid: 'Open', band: '2.4G', rssi: -50, secure: false}
  ]}
}}
;(async () => {
  await p.scanWifi()
  assert.strictEqual(timeout, 16000)
  assert.strictEqual(p.data.wifiList.length, 3)
  assert.strictEqual(p.data.wifiList.find(x => x.ssid === 'Shared').band, '2.4G')
  assert.strictEqual(p.data.wifiList.find(x => x.ssid === 'Five').supported, false)
  p.selectWifi({currentTarget:{dataset:{ssid:'Five'}}})
  assert.ok(p.data.wifiError.includes('2.4GHz'))
  p.selectWifi({currentTarget:{dataset:{ssid:'Open'}}})
  assert.strictEqual(p.data.wifiSelected, 'Open')
  assert.strictEqual(p.data.wifiNeedPassword, false)
})().catch(e => { console.error(e); process.exitCode = 1 })
'''
        subprocess.run(['node', '-e', script], cwd=ROOT, check=True, timeout=10)

    def test_voice_selection_waits_for_device_and_reports_failure(self):
        script = r'''
const assert = require('assert')
let page
global.getApp = () => ({globalData: {}})
global.Page = p => { page = p }
global.wx = {}
require('./wzh-miniprogram/pages/device/device.js')
const p = Object.assign({}, page)
p.data = JSON.parse(JSON.stringify(page.data))
p.setData = changes => { for (const [key,value] of Object.entries(changes)) {
  if (key === 'status.currentVoice') p.data.status.currentVoice = value
  else p.data[key] = value
} }
let message = ''
p.toast = text => { message = text }
p.refreshStatus = () => {}
let resolveRequest
p.client = {request: () => new Promise(resolve => { resolveRequest = resolve })}
const before = p.data.status.currentVoice
p.selectVoice('新音色')
assert.strictEqual(p.data.status.currentVoice, before)
resolveRequest({name: '新音色'})
setImmediate(() => {
  assert.strictEqual(p.data.status.currentVoice, '新音色')
  p.client = {request: () => Promise.reject(new Error('设备忙'))}
  p.selectVoice('另一个音色')
  setImmediate(() => {
    assert.strictEqual(p.data.status.currentVoice, '新音色')
    assert.strictEqual(message, '设备忙')
  })
})
'''
        subprocess.run(['node', '-e', script], cwd=ROOT, check=True, timeout=10)

    def test_failed_upload_releases_device_session(self):
        script = r'''
const assert = require('assert')
const audio = require('./wzh-miniprogram/services/audio')
const ops = []
const client = {request: async op => {
  ops.push(op)
  if (op === 'voice_upload_chunk') throw new Error('disconnected')
  return {chunkLimit: 1024}
}}
audio.uploadPcm(client, {pcm: new Int16Array(80000)}, '测试', () => {})
  .then(() => { throw new Error('upload should fail') })
  .catch(error => {
    assert.strictEqual(error.message, 'disconnected')
    assert.deepStrictEqual(ops, ['voice_upload_begin', 'voice_upload_chunk', 'voice_upload_cancel'])
  })
'''
        subprocess.run(['node', '-e', script], cwd=ROOT, check=True, timeout=10)

    def test_uncertain_cloud_upload_is_not_reported_as_success(self):
        script = r'''
const assert = require('assert')
const audio = require('./wzh-miniprogram/services/audio')
global.setTimeout = fn => { fn(); return 0 }
const client = {request: async op => {
  if (op === 'voice_upload_begin') return {chunkLimit: 2048}
  if (op === 'get_status') return {voice_management_busy: false, voice_management_status: 'uncertain'}
  return {accepted: true}
}}
audio.uploadPcm(client, {pcm: new Int16Array(80000)}, '测试', () => {})
  .then(() => { throw new Error('uncertain result must not be treated as success') })
  .catch(error => { assert.ok(error.message.includes('尚未确认')) })
'''
        subprocess.run(['node', '-e', script], cwd=ROOT, check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()
