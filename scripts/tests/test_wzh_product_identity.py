"""Keep each compiled firmware identity aligned with its mini-program role."""
import json
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


class WzhProductIdentityTest(unittest.TestCase):
    def test_compiled_product_wake_matches_miniprogram(self):
        if not shutil.which('g++') or not shutil.which('node'):
            self.skipTest('g++ and node required')
        products = json.loads(subprocess.check_output(
            ['node', '-e', "console.log(JSON.stringify(require('./wzh-miniprogram/services/products').PRODUCTS))"],
            cwd=ROOT, text=True))
        for board, product in (('wzh_crawlPanda', 'crawlPanda'),
                               ('wzh_dinosaur', 'dinosaur'),
                               ('wzh_tailRedPanda', 'redpanda')):
            with self.subTest(board=board), tempfile.TemporaryDirectory() as directory:
                work = Path(directory)
                (work / 'driver').mkdir()
                (work / 'driver/gpio.h').write_text('typedef int gpio_num_t;\n')
                (work / 'driver/spi_master.h').write_text('typedef int spi_host_device_t;\n')
                shutil.copy(ROOT / 'main/boards/common/wzh/wzh_product.h', work)
                source = (ROOT / 'main/boards' / board / 'wzh_product.cc').read_text()
                (work / 'product.cc').write_text(source)
                macros = set(re.findall(r'= ([A-Z][A-Z0-9_]+),', source))
                (work / 'config.h').write_text(''.join(
                    f'#define {name} ' + ('"TEST"' if name == 'PANDA_AP_PREFIX' else '1') + '\n'
                    for name in sorted(macros)))
                (work / 'test.cc').write_text(
                    '#include "wzh_product.h"\n#include <iostream>\n'
                    'int main(){std::cout << wzh::Product().wake_phrase;}\n')
                subprocess.run(['g++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                                '-I', str(work), str(work / 'product.cc'), str(work / 'test.cc'),
                                '-o', str(work / 'test')], check=True, capture_output=True, text=True)
                self.assertEqual(subprocess.check_output([str(work / 'test')], text=True),
                                 products[product]['wake'])


if __name__ == '__main__':
    unittest.main()
