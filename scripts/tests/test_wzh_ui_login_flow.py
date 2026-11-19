"""Regression tests for the embedded portal's mobile login and guide focus."""
import json
import shutil
import threading
import unittest
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlsplit

try:
    from playwright.sync_api import sync_playwright
except ImportError:
    sync_playwright = None

ROOT = Path(__file__).resolve().parents[2] / 'main/assets/wzh_ui'


class Portal(SimpleHTTPRequestHandler):
    requests = []
    reject = False
    state = 'idle'
    product = 'wzh-crawlpanda'
    cellular = False
    cellular_connected = True
    server_connected = False
    wifi_connected = True
    setup_fail = False
    authenticated = True
    auth_required = False
    scan_empty_requests = 0
    scan_requests = 0

    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(ROOT), **kwargs)

    def log_message(self, *args):
        pass

    def reply(self, payload, status=200):
        body = json.dumps(payload).encode()
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = urlsplit(self.path).path
        if path == '/scan':
            Portal.scan_requests += 1
            if Portal.auth_required:
                return self.reply({'ok': False, 'error': {'message': '请先登录'}}, 401)
            if Portal.scan_empty_requests > 0:
                Portal.scan_empty_requests -= 1
                return self.reply({'ok': True, 'aps': []})
            return self.reply({'ok': True, 'aps': [{'ssid': 'Test WiFi', 'rssi': -48, 'auth': 3}]})
        if path.startswith('/api/'):
            if path == '/api/setup/status':
                return self.reply({'ok': True, 'phase': 'failed', 'error': {'message': '连接失败，请重试'}} if Portal.setup_fail
                                  else {'ok': True, 'phase': 'online', 'ctx': 'done'})
            if path == '/api/user/voices':
                return self.reply({'ok': True, 'has_key': True, 'voices': []})
            if path == '/api/hub/status':
                return self.reply({'hub': {'available': False}})
            if path == '/api/hub/diff':
                return self.reply({'ready': False})
            data = {'ok': True, 'build': Portal.product, 'state': Portal.state,
                    'version': 'test', 'wifi': {'connected': Portal.wifi_connected and not Portal.cellular, 'ssid': 'Home',
                                                'ap_ssid': 'CrawlPanda-ABCD'},
                    'battery': {'available': False}, 'audio': {'volume': 40}}
            if Portal.product == 'wzh-dinosaur':
                data.update({'transport': 'cellular' if Portal.cellular else 'wifi',
                             'transport_switchable': True, 'authenticated': Portal.authenticated, 'saved_network': True})
            if Portal.cellular:
                data['cellular'] = {'connected': Portal.cellular_connected,
                                    'server_connected': Portal.server_connected}
            return self.reply(data)
        self.path = {'/': '/index.html', '/login': '/login.html', '/manual': '/manual.html',
                     '/setup': '/setup.html', '/done': '/done.html',
                     '/developer': '/developer.html'}.get(path, self.path)
        return super().do_GET()

    def do_POST(self):
        raw = self.rfile.read(int(self.headers.get('Content-Length', 0)))
        Portal.requests.append(self.path)
        if Portal.auth_required and self.path in ('/api/network/transport', '/submit'):
            return self.reply({'ok': False, 'error': {'message': '请先登录'}}, 401)
        if Portal.reject:
            return self.reply({'ok': False, 'error': {'message': '密码不正确'}}, 401)
        if self.path == '/api/network/transport':
            Portal.cellular = json.loads(raw)['transport'] == 'cellular'
            return self.reply({'ok': True})
        if self.path == '/submit':
            return self.reply({'ok': True, 'job': 'test-job'})
        return self.reply({'ok': True, 'token': 'test-token'})


@unittest.skipUnless(sync_playwright and shutil.which('google-chrome'), 'Chrome and Playwright required')
class LoginFlowTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = ThreadingHTTPServer(('127.0.0.1', 0), Portal)
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()
        cls.url = 'http://127.0.0.1:' + str(cls.server.server_port)
        cls.playwright = sync_playwright().start()
        cls.browser = cls.playwright.chromium.launch(executable_path=shutil.which('google-chrome'),
                                                       headless=True, args=['--no-sandbox'])

    @classmethod
    def tearDownClass(cls):
        cls.browser.close()
        cls.playwright.stop()
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join(timeout=3)

    def setUp(self):
        Portal.requests.clear()
        Portal.reject = False
        Portal.state = 'idle'
        Portal.product = 'wzh-crawlpanda'
        Portal.cellular = False
        Portal.cellular_connected = True
        Portal.server_connected = False
        Portal.wifi_connected = True
        Portal.setup_fail = False
        Portal.authenticated = True
        Portal.auth_required = False
        Portal.scan_empty_requests = 0
        Portal.scan_requests = 0
        self.page = self.browser.new_page(viewport={'width': 390, 'height': 720}, is_mobile=True, has_touch=True)
        self.errors = []
        self.page.on('pageerror', lambda error: self.errors.append(str(error)))

    def tearDown(self):
        self.page.close()

    def test_product_switch_preserves_theme_and_updates_guide_and_manual(self):
        p = self.page
        p.goto(self.url + '/manual?guide=off')
        p.wait_for_function("document.querySelector('[data-p-ap]').textContent === 'CrawlPanda-ABCD'")
        self.assertEqual(p.locator('[data-p-pass]').inner_text(), 'zylzABCD')
        p.evaluate("PandaTheme.set({theme:'salt'}); PandaGuide.open('manual')")
        for product, name, part in [('crawlPanda', '爬爬熊猫', '双前肢'),
                                    ('redpanda', '摇尾小熊猫', '大尾巴'),
                                    ('dinosaur', '小恐龙', '颈部、尾巴')]:
            p.evaluate('(id) => PandaProduct.set(id, false)', product)
            self.assertEqual(p.evaluate('PandaProduct.id'), product)
            self.assertEqual(p.evaluate('PandaProduct.data.id'), product)
            self.assertEqual(p.locator('.pg-who').inner_text(), name + ' · 使用引导')
            self.assertIn(name, p.title())
            self.assertIn(part, p.locator('[data-p-inline="' + product + '"]').first.inner_text())
            self.assertTrue(p.locator('[data-p-inline="' + product + '"]').first.is_visible())
            for other in {'crawlPanda', 'redpanda', 'dinosaur'} - {product}:
                self.assertFalse(p.locator('[data-p-inline="' + other + '"]').first.is_visible())
            self.assertEqual(p.evaluate('PandaTheme.get().theme'), 'salt')
        self.assertEqual(self.errors, [])

    def test_cellular_online_without_wifi_stays_on_device_page(self):
        Portal.cellular = True
        Portal.product = 'wzh-dinosaur'
        self.page.goto(self.url + '/?guide=off')
        self.page.wait_for_function("document.querySelector('.net-name').textContent === '4G'")
        self.page.wait_for_timeout(3500)
        self.assertEqual(urlsplit(self.page.url).path, '/')
        self.assertEqual(self.errors, [])

    def test_cellular_offline_does_not_redirect_to_wifi_setup(self):
        Portal.cellular = True
        Portal.cellular_connected = False
        Portal.product = 'wzh-dinosaur'
        self.page.goto(self.url + '/?guide=off')
        self.page.wait_for_function("document.querySelector('.net-name').textContent === '4G'")
        self.page.wait_for_timeout(3500)
        self.assertEqual(urlsplit(self.page.url).path, '/')
        self.assertEqual(self.page.locator('.net-sub').first.inner_text(), '网络离线')
        self.assertEqual(self.errors, [])

    def test_cellular_requires_server_handshake_for_connected_label(self):
        Portal.cellular = True
        Portal.product = 'wzh-dinosaur'
        self.page.goto(self.url + '/?guide=off')
        self.page.wait_for_function("document.querySelector('.net-sub').textContent.includes('服务端未连接')")
        Portal.server_connected = True
        self.page.wait_for_function("document.querySelector('.net-sub').textContent === '服务端已连接'")
        Portal.server_connected = False
        self.page.wait_for_function("document.querySelector('.net-sub').textContent.includes('服务端未连接')")
        self.assertEqual(self.errors, [])

    def test_dinosaur_offline_wifi_keeps_details_and_switches_both_ways(self):
        Portal.product = 'wzh-dinosaur'
        Portal.wifi_connected = False
        self.page.goto(self.url + '/?guide=off')
        self.page.locator('.handle').click()
        self.page.locator('#transportSwitch').wait_for(state='visible')
        self.page.wait_for_timeout(3500)
        self.assertEqual(urlsplit(self.page.url).path, '/')
        self.page.locator('[data-transport="cellular"]').click()
        self.page.wait_for_function("document.querySelector('[data-transport=cellular]').disabled")
        self.assertTrue(Portal.cellular)
        self.page.locator('[data-transport="wifi"]').click()
        self.page.wait_for_function("document.querySelector('[data-transport=wifi]').disabled")
        self.assertFalse(Portal.cellular)
        self.assertEqual(self.errors, [])

    def test_setup_switches_from_cellular_and_connection_can_retry(self):
        Portal.product = 'wzh-dinosaur'
        Portal.cellular = True
        Portal.wifi_connected = False
        Portal.setup_fail = True
        self.page.goto(self.url + '/setup?guide=off')
        self.page.get_by_text('Test WiFi', exact=True).click()
        self.assertFalse(Portal.cellular)
        self.assertTrue(self.page.locator('#setupBack').is_visible())
        self.page.locator('#pwInput').fill('password123')
        self.page.locator('#pwConnect').click()
        self.page.get_by_text('连接失败，请重试', exact=True).wait_for()
        self.assertTrue(self.page.locator('#pwConnect').is_enabled())
        Portal.setup_fail = False
        self.page.locator('#pwConnect').click()
        self.page.wait_for_url('**/done')
        self.assertEqual(Portal.requests.count('/submit'), 2)
        self.assertEqual(self.errors, [])

    def test_setup_scan_waits_for_async_firmware_results(self):
        Portal.product = 'wzh-dinosaur'
        Portal.wifi_connected = False
        Portal.scan_empty_requests = 3
        self.page.goto(self.url + '/setup?guide=off')
        self.page.get_by_text('Test WiFi', exact=True).wait_for(timeout=12000)
        self.assertGreaterEqual(Portal.scan_requests, 4)
        self.assertTrue(self.page.locator('#scanBtn').is_enabled())
        self.assertTrue(self.page.locator('#netEmpty').is_hidden())
        self.assertEqual(self.errors, [])

    def test_setup_scan_survives_channel_switch_disconnects(self):
        Portal.product = 'wzh-dinosaur'
        Portal.wifi_connected = False
        self.page.add_init_script('''
          (() => {
            const originalFetch = window.fetch;
            let failures = 2;
            window.fetch = (url, options) => {
              if (url === '/scan' && failures > 0) {
                failures -= 1;
                return Promise.reject(new TypeError('Failed to fetch'));
              }
              return originalFetch(url, options);
            };
          })();
        ''')
        self.page.goto(self.url + '/setup?guide=off')
        self.page.get_by_text('Test WiFi', exact=True).wait_for(timeout=12000)
        self.assertTrue(self.page.locator('#scanBtn').is_enabled())
        self.assertEqual(self.errors, [])

    def test_wifi_submit_timeout_releases_connect_button(self):
        Portal.product = 'wzh-dinosaur'
        Portal.wifi_connected = False
        self.page.goto(self.url + '/setup?guide=off')
        self.page.get_by_text('Test WiFi', exact=True).click()
        self.page.locator('#pwInput').fill('password123')
        self.page.evaluate('''() => {
          window.savedFetch=window.fetch;
          const timer=window.setTimeout;
          window.setTimeout=(fn,ms,...args)=>timer(fn,ms===10000?100:ms,...args);
          window.fetch=(url,options)=>url==='/submit'?new Promise((resolve,reject)=>{
            options.signal.addEventListener('abort',()=>reject(new DOMException('aborted','AbortError')));
          }):window.savedFetch(url,options);
        }''')
        self.page.locator('#pwConnect').click()
        self.page.get_by_text('连接设备超时，请确认手机仍连接设备热点后重试').wait_for()
        self.assertTrue(self.page.locator('#pwConnect').is_enabled())
        self.page.evaluate('window.fetch=window.savedFetch')
        self.page.locator('#pwConnect').click()
        self.page.wait_for_url('**/done')
        self.assertEqual(self.errors, [])

    def test_setup_transport_401_opens_login(self):
        Portal.product = 'wzh-dinosaur'
        Portal.cellular = True
        Portal.wifi_connected = False
        Portal.authenticated = False
        Portal.auth_required = True
        self.page.goto(self.url + '/setup?guide=off')
        self.page.wait_for_url('**/login?next=/setup')
        self.assertEqual(self.errors, [])

    def test_detail_transport_401_opens_login(self):
        Portal.product = 'wzh-dinosaur'
        Portal.wifi_connected = False
        Portal.authenticated = False
        Portal.auth_required = True
        self.page.goto(self.url + '/?guide=off')
        self.page.locator('.handle').click()
        self.page.locator('[data-transport="cellular"]').click()
        self.page.wait_for_url('**/login?next=/')
        self.assertEqual(self.errors, [])

    def test_transport_timeout_reenables_switch_and_retry_works(self):
        Portal.product = 'wzh-dinosaur'
        Portal.wifi_connected = True
        self.page.goto(self.url + '/?guide=off')
        self.page.locator('.handle').click()

        def drop_switch_response():
            self.page.evaluate('''() => {
              window.savedFetch=window.fetch;
              const timer=window.setTimeout;
              window.setTimeout=(fn,ms,...args)=>timer(fn,ms===10000?100:ms,...args);
              let first=true;
              window.fetch=(url,options)=>{
                if(url==='/api/network/transport'&&first){
                  first=false;
                  return new Promise((resolve,reject)=>{
                    options.signal.addEventListener('abort',()=>reject(new DOMException('aborted','AbortError')));
                  });
                }
                return window.savedFetch(url,options);
              };
            }''')
        drop_switch_response()
        self.page.locator('[data-transport="cellular"]').click()
        self.page.get_by_text('网络切换中，请稍候…').wait_for()
        self.page.wait_for_function("!document.querySelector('[data-transport=cellular]').disabled")
        self.page.locator('[data-transport="cellular"]').click()
        self.page.wait_for_function("document.querySelector('[data-transport=cellular]').classList.contains('on')")
        self.assertTrue(Portal.cellular)
        self.assertEqual(self.errors, [])

    def test_offline_dinosaur_setup_can_open_login_for_details(self):
        Portal.product = 'wzh-dinosaur'
        Portal.wifi_connected = False
        Portal.authenticated = False
        self.page.goto(self.url + '/setup?guide=off')
        self.page.locator('#setupBack').click()
        self.page.wait_for_url('**/login?next=/')
        self.assertEqual(self.errors, [])

    def test_product_guide_state_changes_with_device_identity(self):
        p = self.page
        p.goto(self.url + '/manual?guide=off')
        p.evaluate("""() => {
          PandaProduct.set('crawlPanda', false); PandaGuide.reset();
          localStorage.setItem('panda-guide-v1:redpanda', JSON.stringify({seen:{manual:true}}));
          PandaProduct.set('redpanda', false);
        }""")
        self.assertTrue(p.evaluate("PandaGuide.seen('manual')"))
        p.evaluate("PandaProduct.set('crawlPanda', false)")
        self.assertFalse(p.evaluate("PandaGuide.seen('manual')"))
        self.assertEqual(self.errors, [])

    def test_login_does_not_open_guide_or_restore_password_focus(self):
        p = self.page
        p.goto(self.url + '/login')
        p.locator('#loginPw').fill('zylz1234')
        p.wait_for_timeout(1100)
        self.assertEqual(p.locator('.pg-root').count(), 0)
        p.locator('#loginSubmit').click()
        p.wait_for_url(self.url + '/')
        p.wait_for_timeout(1000)
        self.assertIn('/api/login', Portal.requests)
        self.assertNotEqual(p.evaluate('document.activeElement.tagName'), 'INPUT')
        self.assertEqual(self.errors, [])

    def test_login_failure_releases_button_and_can_retry(self):
        p = self.page
        Portal.reject = True
        p.goto(self.url + '/login')
        p.locator('#loginPw').fill('bad')
        p.locator('#loginSubmit').click()
        p.get_by_text('密码不正确').wait_for()
        self.assertTrue(p.locator('#loginSubmit').is_enabled())
        Portal.reject = False
        p.locator('#loginPw').fill('zylz1234')
        p.locator('#loginSubmit').click()
        p.wait_for_url(self.url + '/')
        self.assertEqual(Portal.requests.count('/api/login'), 2)
        self.assertEqual(self.errors, [])

    def test_guide_close_does_not_refocus_text_field(self):
        p = self.page
        p.goto(self.url + '/login')
        p.locator('#loginPw').fill('zylz1234')
        p.evaluate("PandaGuide.open('login')")
        p.locator('.pg-skip').click()
        p.wait_for_timeout(300)
        self.assertNotEqual(p.evaluate('document.activeElement.id'), 'loginPw')
        self.assertEqual(self.errors, [])

    def test_late_guide_does_not_interrupt_input(self):
        p = self.page
        p.goto(self.url + '/login')
        p.locator('#loginPw').fill('zylz1234')
        p.evaluate("PandaGuide.reset(); PandaGuide.auto('login')")
        p.wait_for_timeout(1000)
        self.assertEqual(p.locator('.pg-root').count(), 0)
        self.assertEqual(p.evaluate('document.activeElement.id'), 'loginPw')
        self.assertEqual(self.errors, [])

    def test_timeout_unlocks_login_for_retry(self):
        p = self.page
        p.goto(self.url + '/login')
        p.evaluate("""() => {
          const originalFetch = window.fetch, originalTimer = window.setTimeout;
          window.setTimeout = (fn, ms, ...args) => originalTimer(fn, ms === 15000 ? 100 : ms, ...args);
          window.fetch = (url, options) => url === '/api/login' ? new Promise((resolve, reject) => {
            options.signal.addEventListener('abort', () => reject(new DOMException('aborted', 'AbortError')));
          }) : originalFetch(url, options);
        }""")
        p.locator('#loginPw').fill('zylz1234')
        p.locator('#loginSubmit').click()
        p.get_by_text('连接超时，请确认手机连接设备热点后重试').wait_for()
        self.assertTrue(p.locator('#loginSubmit').is_enabled())
        self.assertEqual(self.errors, [])

    def test_guide_highlight_tracks_targets_and_remains_usable(self):
        p = self.page
        p.goto(self.url + '/?guide=off')
        p.evaluate("PandaGuide.open('detail')")
        p.locator('.pg-title').wait_for()
        p.wait_for_timeout(450)
        for selector in ('.orb-wrap', '.metrics', '.sheet-main > .primary'):
            spot = p.locator('.pg-spot').bounding_box()
            target = p.locator(selector).bounding_box()
            self.assertIsNotNone(spot)
            self.assertIsNotNone(target)
            self.assertAlmostEqual(spot['x'], target['x'] - 5, delta=3)
            self.assertAlmostEqual(spot['y'], target['y'] - 5, delta=3)
            p.locator('.pg-next').click()
            p.wait_for_timeout(450)
        p.locator('.pg-skip').click()
        self.assertEqual(self.errors, [])

    def test_bubbles_preserve_speech_and_reduced_motion_visibility(self):
        p = self.page
        p.goto(self.url + '/?guide=off')
        p.wait_for_timeout(250)
        self.assertEqual(p.locator('.orb-bubbles').evaluate('(el) => getComputedStyle(el).visibility'), 'hidden')
        p.evaluate("document.querySelector('.phone').dataset.state = 'speaking'")
        self.assertEqual(p.locator('.orb-bubbles').evaluate('(el) => getComputedStyle(el).visibility'), 'visible')
        self.assertEqual(p.locator('.orb-foam').evaluate('(el) => getComputedStyle(el).visibility'), 'visible')
        p.emulate_media(reduced_motion='reduce')
        p.evaluate("document.querySelector('.phone').dataset.state = 'idle'")
        self.assertEqual(p.locator('.orb-bubbles').evaluate('(el) => getComputedStyle(el).visibility'), 'visible')
        self.assertEqual(self.errors, [])

    def test_short_mobile_screen_keeps_primary_control_in_view(self):
        p = self.page
        p.set_viewport_size({'width': 320, 'height': 568})
        p.goto(self.url + '/?guide=off')
        p.wait_for_timeout(300)
        button = p.locator('.sheet-main > .primary').bounding_box()
        self.assertIsNotNone(button)
        self.assertLessEqual(button['y'] + button['height'], 568)
        self.assertEqual(p.evaluate('document.documentElement.scrollWidth'), 320)
        self.assertEqual(self.errors, [])

    def test_hidden_voice_animation_stops_and_resumes_when_visible(self):
        p = self.page
        p.add_init_script("""(() => {
          window.framesScheduled = 0;
          window.railStyleReads = 0;
          const originalStyle = window.getComputedStyle;
          window.getComputedStyle = (...args) => {
            if (args[0] && args[0].matches && args[0].matches('[data-fixed-rail]')) window.railStyleReads++;
            return originalStyle(...args);
          };
          const original = window.requestAnimationFrame;
          window.requestAnimationFrame = callback => { window.framesScheduled++; return original(callback); };
        })();""")
        p.goto(self.url + '/?guide=off')
        p.wait_for_timeout(1000)
        before = p.evaluate('window.framesScheduled')
        p.wait_for_timeout(350)
        self.assertLessEqual(p.evaluate('window.framesScheduled') - before, 1)
        p.locator('.handle').click()
        p.locator('[data-fixed-rail]').scroll_into_view_if_needed()
        p.wait_for_timeout(800)
        before = p.evaluate('window.framesScheduled')
        style_before = p.evaluate('window.railStyleReads')
        p.wait_for_timeout(300)
        self.assertGreater(p.evaluate('window.framesScheduled') - before, 1)
        self.assertLessEqual(p.evaluate('window.railStyleReads') - style_before, 1)
        p.locator('.con-close').click()
        p.wait_for_timeout(650)
        before = p.evaluate('window.framesScheduled')
        p.wait_for_timeout(350)
        self.assertLessEqual(p.evaluate('window.framesScheduled') - before, 1)
        self.assertEqual(self.errors, [])

    def test_unchanged_status_does_not_rewrite_metrics_and_changes_still_update(self):
        p = self.page
        p.goto(self.url + '/?guide=off')
        p.wait_for_timeout(700)
        p.evaluate("""() => {
          window.metricMutations = 0;
          new MutationObserver(records => { window.metricMutations += records.length; })
            .observe(document.querySelector('.metrics'), {subtree:true, childList:true, characterData:true, attributes:true});
        }""")
        p.wait_for_timeout(5300)
        self.assertEqual(p.evaluate('window.metricMutations'), 0)
        Portal.state = 'listening'
        p.get_by_text('聆听中', exact=True).first.wait_for(timeout=7000)
        self.assertEqual(self.errors, [])

    def test_developer_login_failure_can_retry(self):
        p = self.page
        Portal.reject = True
        p.goto(self.url + '/developer')
        p.locator('#devPassword').fill('wrong')
        p.locator('#devLogin').click()
        p.get_by_text('密码不正确').wait_for()
        self.assertTrue(p.locator('#devLogin').is_enabled())
        Portal.reject = False
        p.locator('#devPassword').fill('1')
        p.locator('#devLogin').click()
        p.wait_for_function("document.getElementById('devPhone').dataset.state === 'in'")
        self.assertEqual(Portal.requests.count('/api/developer/login'), 2)
        self.assertEqual(self.errors, [])

    def test_developer_login_blurs_input_and_mobile_fits(self):
        p = self.page
        p.goto(self.url + '/developer')
        p.locator('#devPassword').fill('1')
        p.locator('#devLogin').click()
        p.wait_for_function("document.getElementById('devPhone').dataset.state === 'in'")
        self.assertNotEqual(p.evaluate('document.activeElement.id'), 'devPassword')
        self.assertLessEqual(p.locator('#devPhone').bounding_box()['width'], 390)
        self.assertEqual(self.errors, [])
