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
        if path.startswith('/api/'):
            if path == '/api/user/voices':
                return self.reply({'ok': True, 'has_key': True, 'voices': []})
            if path == '/api/hub/status':
                return self.reply({'hub': {'available': False}})
            if path == '/api/hub/diff':
                return self.reply({'ready': False})
            return self.reply({'ok': True, 'build': 'wzh-crawlpanda', 'state': Portal.state,
                               'version': 'test', 'wifi': {'connected': True, 'ssid': 'Home'},
                               'battery': {'available': False}, 'audio': {'volume': 40}})
        self.path = {'/': '/index.html', '/login': '/login.html',
                     '/developer': '/developer.html'}.get(path, self.path)
        return super().do_GET()

    def do_POST(self):
        self.rfile.read(int(self.headers.get('Content-Length', 0)))
        Portal.requests.append(self.path)
        if Portal.reject:
            return self.reply({'ok': False, 'error': {'message': '密码不正确'}}, 401)
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
        self.page = self.browser.new_page(viewport={'width': 390, 'height': 720}, is_mobile=True, has_touch=True)
        self.errors = []
        self.page.on('pageerror', lambda error: self.errors.append(str(error)))

    def tearDown(self):
        self.page.close()

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
