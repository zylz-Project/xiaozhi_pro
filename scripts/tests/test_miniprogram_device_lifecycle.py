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

    def test_guide_is_remembered_per_product_and_reset_only_after_success(self):
        if not shutil.which('node'):
            self.skipTest('node required')
        program = r'''
const assert = require('assert');
let definition, nextTimer = 1;
const timers = new Map(), storage = new Map();
global.Page = value => { definition = value; };
global.setTimeout = fn => { const id = nextTimer++; timers.set(id, fn); return id; };
global.clearTimeout = id => { timers.delete(id); };
global.wx = {
  getWindowInfo: () => ({ windowWidth: 390, windowHeight: 844, statusBarHeight: 20,
                         safeArea: { bottom: 844 } }),
  getStorageSync: key => storage.get(key),
  setStorageSync: (key, value) => storage.set(key, value),
  removeStorageSync: key => storage.delete(key),
  redirectTo() {}
};
global.getApp = () => ({ clearBle() {} });
require('./wzh-miniprogram/pages/device/device.js');
const page = Object.assign({}, definition, {
  data: Object.assign({}, definition.data, { product: { id: 'crawlPanda' }, guideOpen: false }),
  client: {},
  setData(patch, callback) { Object.assign(this.data, patch); if (callback) callback(); },
  openGuide() { this.data.guideOpen = true; },
  syncRailDrift() {},
  stopStatusTimer() {},
  toast() {}
});
function fire() { const [id, fn] = timers.entries().next().value; timers.delete(id); fn(); }
page.onReady();
assert.strictEqual(timers.size, 1);
fire();
assert.strictEqual(page.data.guideOpen, true);
page.closeGuide();
assert.strictEqual(storage.get('panda-guide-v1:crawlPanda'), true);
page.scheduleFirstGuide();
assert.strictEqual(timers.size, 0);
page.data.product = { id: 'dinosaur' };
page.scheduleFirstGuide();
fire();
assert.strictEqual(page.data.guideOpen, true);
page.closeGuide();
assert.strictEqual(storage.get('panda-guide-v1:dinosaur'), true);
page.data.product = { id: 'redpanda' };
page.scheduleFirstGuide();
page.onHide();
assert.strictEqual(timers.size, 0);
(async () => {
  page.data.product = { id: 'dinosaur' };
  page.data.resetReady = true; page.data.resetting = false;
  page.client = { request: async () => { throw new Error('offline'); }, close() {} };
  await page.confirmReset();
  assert.strictEqual(storage.get('panda-guide-v1:dinosaur'), true);
  page.data.resetReady = true;
  page.client = { request: async () => { page._resetDoneResolve({ ok: true }); }, close() {} };
  await page.confirmReset();
  assert.strictEqual(storage.has('panda-guide-v1:dinosaur'), false);
  assert.strictEqual(storage.get('panda-guide-v1:crawlPanda'), true);
})().catch(error => { console.error(error); process.exitCode = 1; });
'''
        subprocess.run(['node', '-e', program], cwd=ROOT, check=True, timeout=15)


if __name__ == '__main__':
    unittest.main()
