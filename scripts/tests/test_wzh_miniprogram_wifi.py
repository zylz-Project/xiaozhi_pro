import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class MiniProgramWifiTest(unittest.TestCase):
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
