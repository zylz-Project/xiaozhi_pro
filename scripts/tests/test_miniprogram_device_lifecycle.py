"""Exercise the actual mini-program page lifecycle with a minimal WeChat stub."""

import shutil
import subprocess
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


class DeviceLifecycleTest(unittest.TestCase):
    def test_startup_notice_appears_persists_across_partial_update_and_clears(self):
        if not shutil.which('node'):
            self.skipTest('node required')
        program = r'''
const assert = require('assert');
let definition;
global.Page = value => { definition = value; };
global.getApp = () => ({ globalData: {} });
require('./wzh-miniprogram/pages/device/device.js');
const page = Object.assign({}, definition, {
 data: Object.assign({}, definition.data),
 setData(patch) { Object.assign(this.data, patch); },
 orbVars() { return ''; }
});
page.applyStatus({startup_notice: '设备温度过高'});
assert.strictEqual(page.data.status.startupNotice, '设备温度过高');
page.applyStatus({volume: 42});
assert.strictEqual(page.data.status.startupNotice, '设备温度过高');
page.applyStatus({startup_notice: ''});
assert.strictEqual(page.data.status.startupNotice, '');
page.applyStatus({onboarding: {done: false, active: false, blocked: true, next_step: 7, step_count: 13}});
assert.strictEqual(page.data.ctaDisabled, true);
assert.strictEqual(page.data.ctaLabel, '请先完成使用教程');
page.applyStatus({onboarding: {done: true, active: false, blocked: false, next_step: 13, step_count: 13}});
assert.strictEqual(page.data.ctaDisabled, false);
page.applyStatus({onboarding: {done: 'false', next_step: 'invalid', step_count: -3}});
assert.strictEqual(page.data.status.onboarding.done, false);
'''
        subprocess.run(['node', '-e', program], cwd=ROOT, check=True, timeout=15)

    def test_show_refreshes_safe_area_and_connected_device(self):
        if not shutil.which('node'):
            self.skipTest('node required')
        program = r'''
const assert = require('assert');
let definition;
global.Page = value => { definition = value; };
global.wx = { getWindowInfo: () => ({ windowWidth: 390, windowHeight: 844,
  statusBarHeight: 20, safeArea: { bottom: 810 } }) };
require('./wzh-miniprogram/pages/device/device.js');
let refreshed = 0, timers = 0;
const page = Object.assign({}, definition, {
  data: Object.assign({}, definition.data, { safeBottom: 0 }),
  client: {},
  setData(patch) { Object.assign(this.data, patch); },
  refreshStatus() { ++refreshed; },
  startStatusTimer() { ++timers; }
});
page.onShow();
assert.strictEqual(page.data.safeBottom, 34);
assert.strictEqual(refreshed, 1);
assert.strictEqual(timers, 1);
page.client = null;
page.data.safeBottom = 0;
page.onShow();
assert.strictEqual(page.data.safeBottom, 34);
assert.strictEqual(refreshed, 1);
assert.strictEqual(timers, 1);
'''
        subprocess.run(['node', '-e', program], cwd=ROOT, check=True, timeout=15)

    def test_guide_is_remembered_per_device_and_reset_only_after_success(self):
        if not shutil.which('node'):
            self.skipTest('node required')
        program = r'''
const assert = require('assert');
let definition, nextTimer = 1;
const timers = new Map();
global.Page = value => { definition = value; };
global.setTimeout = fn => { const id = nextTimer++; timers.set(id, fn); return id; };
global.clearTimeout = id => { timers.delete(id); };
global.wx = {
  getStorageSync() { throw new Error('guide must use the device'); },
  setStorageSync() { throw new Error('guide must use the device'); }
};
global.getApp = () => ({ globalData: {} });
require('./wzh-miniprogram/pages/device/device.js');
const deviceDone = new Map();
function open(identity, epoch) {
  const key = identity + ':' + epoch;
  const page = Object.assign({}, definition, {
    data: JSON.parse(JSON.stringify(definition.data)),
    client: { device: { identity, guide_epoch: epoch }, request: async (op, data) => {
      assert.strictEqual(op, 'guide_complete');
      assert.strictEqual(data.epoch, epoch);
      deviceDone.set(key, true);
    } },
    setData(patch, callback) { Object.assign(this.data, patch); if (callback) callback(); },
    startDeviceGuide(kind) { this.data.guideOpen = true; this.data.guideKind = kind; },
    syncRailDrift() {}
  });
  page.syncDevice();
  page.onReady();
  page.applyStatus({ online: true, wifi: { configured: true }, guide_done: deviceDone.get(key) === true });
  for (const [id, fn] of timers) { timers.delete(id); fn(); }
  return page;
}
(async () => {
  const first = open('unit-a', 4);
  assert.strictEqual(first.data.guideKind, 'first');
  await first.closeGuide(); // Explicit skip also applies across phones.
  assert.strictEqual(first.data.guideOpen, false);
  assert.strictEqual(open('unit-a', 4).data.guideOpen, false);
  assert.strictEqual(open('unit-b', 4).data.guideOpen, true);
  assert.strictEqual(open('unit-a', 5).data.guideOpen, true);
})().catch(error => { console.error(error); process.exitCode = 1; });
'''
        subprocess.run(['node', '-e', program], cwd=ROOT, check=True, timeout=15)


if __name__ == '__main__':
    unittest.main()
