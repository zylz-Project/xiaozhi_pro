"""Exercise the actual mini-program page lifecycle with a minimal WeChat stub."""

import shutil
import subprocess
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


class DeviceLifecycleTest(unittest.TestCase):
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


if __name__ == '__main__':
    unittest.main()
