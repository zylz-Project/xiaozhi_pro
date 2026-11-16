import json
import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]


class WzhMemoryProfileTest(unittest.TestCase):
    def test_all_product_variants_reserve_internal_memory_for_network(self):
        for board in ('wzh_crawlPanda', 'wzh_dinosaur', 'wzh_tailRedPanda'):
            for filename in ('config.json', 'config.miniprogram-experimental.json'):
                config = json.loads((ROOT / 'main/boards' / board / filename).read_text())
                for build in config['builds']:
                    with self.subTest(board=board, config=filename, build=build['name']):
                        options = set(build['sdkconfig_append'])
                        self.assertIn('CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL=y', options)
                        self.assertIn('CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_INTERNAL=n', options)
                        self.assertIn('CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=256', options)
                        self.assertNotIn('CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_INTERNAL=y', options)
                        self.assertNotIn('CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y', options)


if __name__ == '__main__':
    unittest.main()
