"""Regression tests for the embedded portal's mobile login and guide focus."""
import io
import json
import shutil
import threading
import unittest
import wave
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlsplit

try:
    from playwright.sync_api import sync_playwright
except ImportError:
    sync_playwright = None

ROOT = Path(__file__).resolve().parents[2] / 'main/assets/wzh_ui'
DEVELOPER_PASSWORD = 'zeyuanlingzhi'


class Portal(SimpleHTTPRequestHandler):
    requests = []
    reject = False
    state = 'idle'
    product = 'wzh-crawlpanda'
    cellular = False
    cellular_connected = True
    transport_switch = None
    server_connected = False
    wifi_connected = True
    setup_fail = False
    authenticated = True
    auth_required = False
    scan_empty_requests = 0
    scan_requests = 0
    open_network = False
    enter_expired = False
    setup_password = None
    setup_job = None

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
            return self.reply({'ok': True, 'aps': [{'ssid': 'Test WiFi', 'rssi': -48,
                                                    'auth': 0 if Portal.open_network else 3}]})
        if path.startswith('/api/'):
            if path == '/api/setup/status':
                if parse_qs(urlsplit(self.path).query).get('job') != [Portal.setup_job]:
                    return self.reply({'ok': False, 'error': {'message': '配网任务不存在'}}, 401)
                return self.reply({'ok': True, 'phase': 'failed', 'error': {'message': '连接失败，请重试'}} if Portal.setup_fail
                                  else {'ok': True, 'phase': 'online', 'net_state': 'ONLINE', 'ctx': 'done'})
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
                if Portal.transport_switch:
                    data['transport_switch'] = Portal.transport_switch
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
        if self.path == '/api/developer/login':
            if json.loads(raw).get('password') != DEVELOPER_PASSWORD:
                return self.reply({'ok': False, 'error': 'invalid_password'}, 403)
            return self.reply({'token': 'test-token', 'expires_in_s': 1800})
        if Portal.reject:
            return self.reply({'ok': False, 'error': {'message': '密码不正确'}}, 401)
        if self.path == '/api/network/transport':
            Portal.cellular = json.loads(raw)['transport'] == 'cellular'
            return self.reply({'ok': True})
        if self.path == '/submit':
            data = json.loads(raw)
            Portal.setup_password = data['password']
            Portal.setup_job = data.get('job') or 'test-job'
            return self.reply({'ok': True, 'job': Portal.setup_job})
        if self.path == '/api/setup/status':
            return self.reply({'ok': True, 'phase': 'online', 'net_state': 'ONLINE', 'ctx': 'done'})
        if self.path == '/api/enter' and Portal.enter_expired:
            return self.reply({'ok': False, 'error': {'message': '完成页已失效'}}, 401)
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
        Portal.transport_switch = None
        Portal.server_connected = False
        Portal.wifi_connected = True
        Portal.setup_fail = False
        Portal.authenticated = True
        Portal.auth_required = False
        Portal.scan_empty_requests = 0
        Portal.scan_requests = 0
        Portal.open_network = False
        Portal.enter_expired = False
        Portal.setup_password = None
        Portal.setup_job = None
        self.page = self.browser.new_page(viewport={'width': 390, 'height': 720}, is_mobile=True, has_touch=True)
        self.errors = []
        self.page.on('pageerror', lambda error: self.errors.append(str(error)))

    def tearDown(self):
        self.page.close()

    def test_product_switch_preserves_theme_and_updates_guide_and_manual(self):
        p = self.page
        p.goto(self.url + '/manual?guide=off')
        p.wait_for_function("document.querySelector('[data-p-ap]').textContent === 'CrawlPanda-ABCD'")
        self.assertEqual(p.locator('[data-p-version]').inner_text(), 'test')
        self.assertNotIn('zylzABCD', p.locator('#m-network').inner_text())
        p.evaluate("PandaTheme.set({theme:'salt'}); PandaGuide.open('manual')")
        for product, name, part, wake in [('crawlPanda', '爬爬熊猫', '双前肢', '小华'),
                                          ('redpanda', '摇尾小熊猫', '大尾巴', '小武'),
                                          ('dinosaur', '小恐龙', '颈部 2 路', '小威')]:
            p.evaluate('(id) => PandaProduct.set(id, false)', product)
            self.assertEqual(p.evaluate('PandaProduct.id'), product)
            self.assertEqual(p.evaluate('PandaProduct.data.id'), product)
            self.assertEqual(p.locator('.pg-who').inner_text(), name + ' · 使用引导')
            self.assertIn(name, p.title())
            current_parts = p.locator('#m-parts [data-p-block="' + product + '"]')
            self.assertIn(part, current_parts.inner_text())
            self.assertTrue(current_parts.is_visible())
            for other in {'crawlPanda', 'redpanda', 'dinosaur'} - {product}:
                self.assertFalse(p.locator('#m-parts [data-p-block="' + other + '"]').is_visible())
            commands = p.locator('#m-offline-commands')
            self.assertEqual(commands.locator('[data-p-role]').inner_text(), wake)
            for group in commands.locator('details:visible').all():
                group.evaluate('(el) => { el.open = true; }')
            phrases = commands.inner_text()
            self.assertIn('陪我玩', phrases)
            self.assertEqual('挥挥左爪' in phrases, product == 'crawlPanda')
            self.assertEqual('尾巴画八字' in phrases, product != 'crawlPanda')
            self.assertEqual('脖子向前探' in phrases, product == 'dinosaur')
            self.assertEqual(p.evaluate('PandaTheme.get().theme'), 'salt')
        self.assertEqual(self.errors, [])

    def test_manual_works_offline_and_shows_current_product_instructions(self):
        p = self.page
        p.set_viewport_size({'width': 320, 'height': 568})
        Portal.product = 'wzh-dinosaur'
        Portal.wifi_connected = False
        Portal.authenticated = False
        p.goto(self.url + '/manual#m-motion')
        p.get_by_text('小龙小龙', exact=True).wait_for()
        self.assertIn('小威', p.locator('#m-chat').inner_text())
        p.get_by_role('link', name='离线口令', exact=True).click()
        self.assertEqual(p.evaluate('location.hash'), '#m-offline-commands')
        commands = p.locator('#m-offline-commands')
        self.assertIn('小威', commands.inner_text())
        commands.get_by_text('颈部与点头', exact=True).click()
        self.assertTrue(commands.get_by_text('把头低一点', exact=False).is_visible())
        self.assertIn('30 秒', p.locator('#m-motion').inner_text())
        self.assertIn('背部顺摸', p.locator('#m-controls').inner_text())
        self.assertEqual(p.evaluate('document.documentElement.scrollWidth'), 320)
        self.assertFalse(p.locator('.pg-root').is_visible())
        self.assertEqual(self.errors, [])

    def test_embedded_manual_escape_restores_focus(self):
        p = self.page
        p.goto(self.url + '/?guide=off')
        p.locator('.handle').click()
        opener = p.locator('.con-manual')
        self.assertIsNone(p.locator('.manual-frame').get_attribute('src'))
        opener.click()
        frame = p.frame_locator('.manual-frame')
        frame.get_by_role('link', name='动作', exact=True).press('Escape')
        self.assertEqual(opener.get_attribute('aria-expanded'), 'false')
        self.assertTrue(opener.evaluate('(el) => document.activeElement === el'))
        self.assertTrue(p.locator('.phone').evaluate("(el) => el.classList.contains('expanded')"))
        self.assertEqual(self.errors, [])

    def test_manual_directory_stays_visible_and_jumps_to_sections(self):
        page = self.page
        page.set_default_timeout(5000)
        for build, toy in [('wzh-crawlpanda', 'crawlPanda'), ('wzh-tailredpanda', 'redpanda'),
                           ('wzh-dinosaur', 'dinosaur')]:
            Portal.product = build
            for embedded in (False, True):
                with self.subTest(product=toy, embedded=embedded):
                    page.set_viewport_size({'width': 320, 'height': 568})
                    if embedded:
                        page.goto(self.url + '/?guide=off')
                        page.locator('.handle').click()
                        page.locator('.con-manual').click()
                        view = page.frame_locator('.manual-frame')
                    else:
                        page.goto(self.url + '/manual?guide=off')
                        view = page
                    view.locator(f'html[data-toy="{toy}"]').wait_for()
                    directory = view.get_by_role('navigation', name='说明书章节')
                    chapter_links = directory.locator('.manual-nav-links')
                    content = view.locator('.manual-scroll')
                    top = directory.bounding_box()['y']
                    self.assertTrue(chapter_links.evaluate('(el) => el.scrollWidth > el.clientWidth'))
                    if embedded:
                        links = chapter_links.bounding_box()
                        close = page.get_by_role('button', name='关闭说明书').bounding_box()
                        self.assertLessEqual(links['x'] + links['width'], close['x'])

                    content.evaluate('(el) => { el.scrollTop = el.scrollHeight; }')
                    self.assertGreater(content.evaluate('(el) => el.scrollTop'), 0)
                    self.assertAlmostEqual(directory.bounding_box()['y'], top, delta=1)
                    chapter_links.evaluate('(el) => { el.scrollLeft = el.scrollWidth; }')
                    directory.get_by_role('link', name='常见问题').click()
                    heading = view.locator('#m-faq h2').bounding_box()
                    viewport = content.bounding_box()
                    self.assertGreaterEqual(heading['y'], viewport['y'])
                    self.assertLess(heading['y'] + heading['height'], viewport['y'] + viewport['height'])
                    directory.get_by_role('link', name='快速上手').press('Enter')
                    self.assertLess(content.evaluate('(el) => el.scrollTop'), 300)
                    self.assertAlmostEqual(directory.bounding_box()['y'], top, delta=1)
        self.assertEqual(self.errors, [])

    def test_manual_on_short_desktop_keeps_scrolling_inside_the_manual(self):
        page = self.page
        page.set_viewport_size({'width': 1024, 'height': 600})
        page.goto(self.url + '/manual?guide=off#m-faq')
        directory = page.get_by_role('navigation', name='说明书章节')
        self.assertGreaterEqual(directory.bounding_box()['y'], 0)
        self.assertEqual(page.evaluate('document.scrollingElement.scrollHeight'), 600)
        self.assertGreater(page.locator('.manual-scroll').evaluate('(el) => el.scrollTop'), 0)
        self.assertEqual(self.errors, [])

    def test_open_network_can_finish_setup_and_enter_details(self):
        p = self.page
        Portal.open_network = True
        attempts = []
        def enter(route):
            attempts.append(True)
            if len(attempts) == 1:
                route.abort('connectionreset')
            else:
                route.continue_()
        p.route('**/api/enter', enter)
        p.goto(self.url + '/setup?guide=off')
        p.get_by_text('Test WiFi', exact=True).click()
        p.get_by_text('这是开放网络，可直接连接').wait_for()
        p.locator('#pwInput').press('Enter')
        p.get_by_text('网络连接成功').wait_for()
        self.assertNotIn('/api/setup/status', Portal.requests)
        self.assertEqual(Portal.setup_password, '')
        p.locator('#donePrimary').click()
        p.wait_for_url(self.url + '/')
        self.assertEqual(Portal.requests.count('/api/enter'), 1)
        self.assertEqual(len(attempts), 2)
        self.assertEqual(self.errors, [])

    def test_setup_recovers_after_the_first_status_request_loses_connection(self):
        p = self.page
        p.add_init_script('''
          const portalFetch = window.fetch.bind(window);
          window.fetch = (input, options) => {
            if (input === '/api/status' && options?.signal && !window.portalRejoined) {
              return Promise.reject(new TypeError('Connection reset'));
            }
            return portalFetch(input, options);
          };
        ''')
        p.goto(self.url + '/setup?guide=off')
        p.get_by_text('正在恢复与设备的连接…').wait_for()
        p.evaluate("window.portalRejoined = true; window.dispatchEvent(new Event('pageshow'))")
        p.get_by_text('Test WiFi', exact=True).wait_for(timeout=4000)
        self.assertEqual(Portal.scan_requests, 1)
        self.assertTrue(p.locator('#scanBtn').is_enabled())
        self.assertEqual(self.errors, [])

    def test_detail_keeps_last_status_when_a_response_is_truncated(self):
        p = self.page
        p.goto(self.url + '/?guide=off')
        p.wait_for_function("document.querySelector('.net-name').textContent === 'Home'")
        p.route('**/api/status', lambda route: route.fulfill(
            status=200, content_type='application/json', body='{"ok":true,'))
        resume = "window.dispatchEvent(new PageTransitionEvent('pageshow', {persisted: true}))"
        p.evaluate(resume)
        p.locator('.status-text').get_by_text('正在恢复连接…', exact=True).wait_for(timeout=3000)
        self.assertEqual(p.locator('.net-name').first.inner_text(), 'Home')
        self.assertEqual(urlsplit(p.url).path, '/')
        p.unroute('**/api/status')
        Portal.state = 'listening'
        p.evaluate(resume)
        p.wait_for_function("document.querySelector('.status-text').textContent === '聆听中'")
        self.assertEqual(self.errors, [])

    def test_detail_reloads_stale_html_for_another_product(self):
        p = self.page
        Portal.product = 'wzh-tailredpanda'
        pages = []

        def rewrite_document(route):
            pages.append(route.request.url)
            response = route.fetch()
            product = 'redpanda' if '_device=redpanda' in route.request.url else 'crawlPanda'
            body = response.text().replace('<html ', f'<html data-build-product="{product}" ', 1)
            route.fulfill(response=response, body=body)

        p.route(lambda url: urlsplit(url).path == '/', rewrite_document)
        p.goto(self.url + '/?guide=off')
        p.wait_for_function("document.querySelector('[data-p-name]').textContent === '摇尾小熊猫'")
        self.assertEqual(len(pages), 2)
        self.assertIn('_device=redpanda', pages[-1])
        self.assertEqual(self.errors, [])

    def test_detail_retries_once_if_a_stylesheet_fails_to_load(self):
        p = self.page
        pages = []
        p.on('request', lambda request: pages.append(request.url)
             if request.is_navigation_request() else None)
        p.route('**/css/index.css', lambda route: route.abort('connectionreset'), times=1)
        p.goto(self.url + '/?guide=off')
        p.wait_for_function("""() => {
          const sheet = Array.from(document.styleSheets).find(s => s.href?.includes('/css/index.css'));
          try { return sheet && sheet.cssRules.length > 0; } catch { return false; }
        }""")
        self.assertFalse(p.locator('#portalLoadError').is_visible())
        self.assertEqual(len(pages), 2)
        self.assertIn('_assets_retry=1', pages[1])
        self.assertEqual(self.errors, [])

    def test_detail_requests_live_status_before_the_decorative_logo(self):
        p = self.page
        requests = []
        p.on('request', lambda request: requests.append(urlsplit(request.url).path))
        p.goto(self.url + '/?guide=off')
        p.wait_for_function("document.querySelector('.brand-mark').naturalWidth > 1")
        self.assertLess(requests.index('/api/status'), requests.index('/assets/zeyuan-logo.webp'))
        self.assertEqual(self.errors, [])

    def test_detail_voice_audition_reports_result_without_blocking_chat(self):
        page = self.page
        Portal.state = 'listening'
        voice = {'id': 'xiaozhi-native', 'name': '原声', 'current': True, 'builtin': True}
        catalog_loads, auditions, phases = [], [], ['queued', 'playing', 'failed']

        def catalog(route):
            catalog_loads.append(True)
            route.fulfill(json={'ok': True, 'has_key': True, 'voices': [voice]})

        def audition(route):
            auditions.append(route.request.post_data_json['id'])
            route.fulfill(status=202, json={'ok': True})

        page.route('**/api/user/voices', catalog)
        page.route('**/api/user/voices/audition', audition)
        page.route('**/api/user/voices/audition/status',
                   lambda route: route.fulfill(json={'phase': phases.pop(0)}))
        page.goto(self.url + '/?guide=off')
        page.locator('.voice-meta b').get_by_text('原声', exact=True).wait_for()
        page.locator('.voice-try').click()
        page.get_by_text('原声会在正常对话中播放', exact=True).wait_for()
        self.assertEqual(auditions, [])

        voice.update(id='longanhuan_v3.1', name='龙安欢')
        resume = "window.dispatchEvent(new PageTransitionEvent('pageshow', {persisted: true}))"
        page.evaluate(resume)
        page.locator('.voice-meta b').get_by_text('龙安欢', exact=True).wait_for()
        prior_loads = len(catalog_loads)
        page.locator('.voice-try').click()
        page.get_by_text('正在准备试听', exact=True).wait_for()
        page.evaluate(resume)
        with page.expect_response('**/api/user/chat/stop'):
            page.locator('.sheet-main > .primary').click()
        self.assertIn('/api/user/chat/stop', Portal.requests)
        page.get_by_text('试听失败，请检查网络和语音服务', exact=True).wait_for(timeout=5000)
        self.assertEqual(auditions, ['longanhuan_v3.1'])
        self.assertEqual(len(catalog_loads), prior_loads)
        self.assertEqual(self.errors, [])

    def test_new_voice_can_be_renamed_and_deleted_with_visible_cloud_result(self):
        p = self.page
        clone = {'id':'clone-new','name':'新音色','current':True,'builtin':False}
        voices = [{'id':'xiaozhi-native','name':'原声','current':False,'builtin':True}, clone]
        operations, pending = [], {}

        def submit(route):
            action = urlsplit(route.request.url).path.rsplit('/', 1)[-1]
            payload = route.request.post_data_json
            operations.append((action,payload['id'],payload['name']))
            pending.update(ticket=len(operations), action=action, name=payload['name'], polls=0)
            route.fulfill(status=202, json={'ok':True,'request':pending['ticket']})

        def status(route):
            pending['polls'] += 1
            result = 'busy' if pending['polls'] == 1 else 'network' if pending['ticket'] == 2 else 'ok'
            if result == 'ok':
                if pending['action'] == 'rename':
                    clone['name'] = pending['name']
                else:
                    voices[:] = voices[:1]
                    voices[0]['current'] = True
            route.fulfill(json={'voice_operations':[{'request':pending['ticket'],'status':result}]})

        p.route('**/api/user/voices', lambda r: r.fulfill(json={'has_key':True,'voices':voices}))
        p.route('**/api/user/voices/rename', submit)
        p.route('**/api/user/voices/delete', submit)
        p.route('**/api/user/voices/manage/status', status)
        p.add_init_script("window.prompt=window.confirm=()=>{throw Error('Native dialog unavailable');}")
        p.goto(self.url + '/?guide=off')
        p.locator('.handle').click()
        p.get_by_role('button', name='管理复刻音色').click()
        p.get_by_role('button', name='重命名 新音色').click()
        dialog = p.locator('.wzh-voice-dialog')
        self.assertEqual(dialog.get_by_role('textbox').input_value(), '新音色')
        p.keyboard.press('Escape')
        dialog.wait_for(state='detached')
        self.assertFalse(operations)
        p.get_by_role('button', name='重命名 新音色').click()
        dialog.get_by_role('textbox').fill('家人')
        dialog.get_by_role('button', name='保存').click()
        status_text = p.locator('[data-voice-operation-status]')
        status_text.get_by_text('正在保存名称…', exact=True).wait_for()
        p.get_by_role('button', name='删除 家人').wait_for()
        p.get_by_role('button', name='删除 家人').click()
        self.assertFalse(dialog.get_by_role('textbox').is_visible())
        dialog.get_by_role('button', name='删除', exact=True).click()
        status_text.get_by_text('无法完成云端操作，请检查网络和语音服务。').wait_for()
        p.get_by_role('button', name='删除 家人').click()
        dialog.get_by_role('button', name='删除', exact=True).click()
        status_text.get_by_text('删除音色成功', exact=True).wait_for()
        self.assertEqual(p.locator('[data-vm-list] .vm-row').count(), 0)
        self.assertEqual(operations, [('rename','clone-new','家人'),('delete','clone-new',''),('delete','clone-new','')])
        self.assertEqual(self.errors, [])

    def test_voice_upload_uses_its_own_result_after_an_unrelated_success(self):
        page = self.page
        polls = []

        def status(route):
            polls.append(True)
            operations = [{'request': 8, 'status': 'ok'}]
            if len(polls) > 1:
                operations.append({'request': 7, 'status': 'network'})
            route.fulfill(json={'voice_operations': operations, 'voice_management_status': 'ok'})

        page.route('**/api/user/voices/clone', lambda r: r.fulfill(
            status=202, json={'ok': True, 'request': 7}))
        page.route('**/api/user/voices/manage/status', status)
        wave_file = io.BytesIO()
        with wave.open(wave_file, 'wb') as recording:
            recording.setparams((1, 2, 16000, 0, 'NONE', 'not compressed'))
            recording.writeframes(b'\0\0' * (16000 * 6))
        page.goto(self.url + '/?guide=off')
        page.locator('.handle').click()
        page.locator('.lab-upload').click()
        page.locator('#audio-file-input').set_input_files(
            {'name': '测试.wav', 'mimeType': 'audio/wav', 'buffer': wave_file.getvalue()})
        page.locator('[data-create-audio]').click()
        page.locator('[data-voice-operation]').get_by_text('音色服务连接失败，请检查网络后重试。').wait_for()
        self.assertGreaterEqual(len(polls), 2)
        self.assertEqual(self.errors, [])

    def test_expired_completion_returns_to_login(self):
        p = self.page
        Portal.enter_expired = True
        p.goto(self.url + '/done')
        p.get_by_text('网络连接成功').wait_for()
        p.locator('#donePrimary').click()
        p.get_by_text('配网完成页已失效').wait_for()
        p.get_by_role('button', name='前往登录').click()
        p.wait_for_url('**/login?next=/')
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

    def test_switch_from_offline_cellular_recovers_a_lost_hotspot_response(self):
        Portal.product = 'wzh-dinosaur'
        Portal.cellular = True
        Portal.cellular_connected = False
        Portal.wifi_connected = False
        posts = []
        failed_reads = []
        p = self.page

        def accept_then_drop(route):
            posts.append(route.request.post_data_json)
            Portal.transport_switch = {'id': 1, 'state': 'pending', 'target': 'wifi'}
            route.abort()

        def hotspot_status(route):
            if posts and len(failed_reads) < 2:
                failed_reads.append(True)
                route.abort()
            else:
                route.continue_()

        p.route('**/api/network/transport', accept_then_drop)
        p.route('**/api/status', hotspot_status)
        p.goto(self.url + '/?guide=off')
        p.locator('.handle').click()
        p.locator('[data-transport=wifi]').click()
        p.get_by_text('正在确认 Wi-Fi 切换进度…', exact=True).wait_for()
        p.wait_for_function("document.querySelector('.ts-hint').textContent.includes('连接暂时中断')")
        self.assertTrue(p.locator('[data-transport=wifi]').is_disabled())
        self.assertTrue(p.locator('[data-transport=cellular]').is_disabled())
        Portal.cellular = False
        Portal.transport_switch = {'id': 1, 'state': 'succeeded', 'target': 'wifi'}
        p.get_by_text('已切到 Wi-Fi，等待联网', exact=True).wait_for()
        self.assertTrue(p.locator('[data-transport=cellular]').is_enabled())
        self.assertEqual(posts, [{'transport': 'wifi'}])
        self.assertEqual(self.errors, [])

    def test_setup_reports_switch_failure_until_explicit_retry(self):
        Portal.product = 'wzh-dinosaur'
        Portal.cellular = True
        Portal.cellular_connected = False
        Portal.wifi_connected = False
        posts = []
        p = self.page

        def switch(route):
            posts.append(route.request.post_data_json)
            Portal.transport_switch = {'id': len(posts), 'state': 'pending', 'target': 'wifi'}
            route.fulfill(status=202, json={'ok': True, 'transport_switch': Portal.transport_switch})

        p.route('**/api/network/transport', switch)
        p.goto(self.url + '/setup?guide=off')
        p.get_by_text('正在确认 Wi-Fi 切换进度…', exact=True).wait_for()
        Portal.transport_switch = {'id': 1, 'state': 'failed', 'target': 'wifi',
                                   'message': '切换到 Wi-Fi 失败，可以重试'}
        p.get_by_text('切换到 Wi-Fi 失败，可以重试', exact=True).wait_for()
        p.evaluate("window.dispatchEvent(new Event('pageshow'))")
        p.wait_for_timeout(700)
        self.assertEqual(len(posts), 1)
        self.assertEqual(Portal.scan_requests, 0)
        p.locator('#scanBtn').click()
        p.get_by_text('正在确认 Wi-Fi 切换进度…', exact=True).wait_for()
        Portal.cellular = False
        Portal.transport_switch = {'id': 2, 'state': 'succeeded', 'target': 'wifi'}
        p.get_by_text('Test WiFi', exact=True).wait_for()
        self.assertEqual(posts, [{'transport': 'wifi'}, {'transport': 'wifi'}])
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

    def test_setup_recovers_when_access_point_change_drops_submit_response(self):
        page = self.page

        def drop_submit_response(route):
            response = route.fetch()
            self.assertTrue(response.ok)
            route.abort()

        page.route('**/submit', drop_submit_response)
        page.goto(self.url + '/setup?guide=off')
        page.get_by_text('Test WiFi', exact=True).click()
        page.locator('#pwInput').fill('example-pass')
        page.locator('#pwConnect').click()
        page.wait_for_url('**/done')
        self.assertRegex(Portal.setup_job, r'^[0-9a-f]{32}$')
        self.assertEqual(Portal.requests.count('/submit'), 1)
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
        p = self.page
        p.goto(self.url + '/?guide=off')
        p.locator('.handle').click()
        p.evaluate('''() => {
          const fetch = window.fetch.bind(window);
          let first = true;
          window.fetch = (url, options) => {
            if (url === '/api/network/transport' && first) {
              first = false;
              return new Promise((resolve, reject) => {
                options.signal.addEventListener('abort', () => {
                  window.switchRequestAborted = true;
                  reject(new DOMException('aborted', 'AbortError'));
                });
              });
            }
            return fetch(url, options);
          };
        }''')
        requests = []

        def accept_retry(route):
            requests.append(route.request.post_data_json)
            Portal.transport_switch = {'id': 1, 'state': 'pending', 'target': 'cellular'}
            route.fulfill(status=202, json={'ok': True, 'transport_switch': Portal.transport_switch})

        p.route('**/api/network/transport', accept_retry)
        cellular = p.locator('[data-transport="cellular"]')
        cellular.click()
        p.wait_for_function('window.switchRequestAborted === true')
        p.get_by_text('未能确认切换请求，请保持手机连接设备热点后重试', exact=True).wait_for()
        self.assertTrue(cellular.is_enabled())
        self.assertFalse(Portal.cellular)
        self.assertEqual(requests, [])

        cellular.click()
        p.get_by_text('正在确认 4G 切换进度…', exact=True).wait_for()
        self.assertTrue(cellular.is_disabled())
        self.assertEqual(cellular.get_attribute('aria-pressed'), 'false')
        Portal.cellular = True
        Portal.transport_switch = {'id': 1, 'state': 'succeeded', 'target': 'cellular'}
        p.get_by_text('已切到 4G', exact=True).wait_for()
        self.assertEqual(cellular.get_attribute('aria-pressed'), 'true')
        self.assertEqual(requests, [{'transport': 'cellular'}])
        self.assertEqual(self.errors, [])

    def test_offline_selected_cellular_rechecks_once_and_waits_for_new_result(self):
        Portal.product = 'wzh-dinosaur'
        Portal.cellular = True
        Portal.cellular_connected = False
        Portal.transport_switch = {'id': 7, 'state': 'succeeded', 'target': 'cellular'}
        posts = []
        p = self.page

        def accept_recheck(route):
            posts.append(route.request.post_data_json)
            job = {'id': 8, 'state': 'pending', 'target': 'cellular'}
            route.fulfill(status=202, json={'ok': True, 'transport_switch': job})

        p.route('**/api/network/transport', accept_recheck)
        p.goto(self.url + '/?guide=off')
        p.locator('.handle').click()
        p.get_by_text('4G 未联网，可点 4G 重新检测或切到 Wi-Fi', exact=True).wait_for()
        cellular = p.locator('[data-transport="cellular"]')
        self.assertTrue(cellular.is_enabled())
        self.assertEqual(cellular.get_attribute('aria-pressed'), 'true')
        cellular.click()
        progress = p.get_by_text('正在确认 4G 重新检测进度…', exact=True)
        progress.wait_for()
        p.wait_for_timeout(800)
        self.assertTrue(progress.is_visible())
        self.assertTrue(cellular.is_disabled())
        Portal.transport_switch = {'id': 8, 'state': 'succeeded', 'target': 'cellular'}
        p.get_by_text('已请求重新检测 4G。可点 4G 重新检测', exact=True).wait_for()
        self.assertTrue(cellular.is_enabled())
        Portal.cellular_connected = True
        p.wait_for_function("document.querySelector('[data-transport=cellular]').disabled")
        self.assertEqual(posts, [{'transport': 'cellular'}])
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

    def test_login_keyboard_requires_tap_and_is_reset_on_history_restore(self):
        p = self.page
        p.goto(self.url + '/login')
        password = p.locator('#loginPw')
        self.assertNotEqual(p.evaluate('document.activeElement.id'), 'loginPw')
        password.focus()
        password.dispatch_event('click')
        self.assertEqual(password.get_attribute('inputmode'), 'none')
        password.tap()
        self.assertEqual(password.get_attribute('inputmode'), 'text')
        self.assertEqual(p.evaluate('document.activeElement.id'), 'loginPw')
        password.fill('zylz1234')
        p.evaluate("dispatchEvent(new PageTransitionEvent('pagehide', {persisted:true}))")
        self.assertEqual(password.get_attribute('inputmode'), 'none')
        self.assertNotEqual(p.evaluate('document.activeElement.id'), 'loginPw')
        password.focus()
        p.evaluate("dispatchEvent(new PageTransitionEvent('pageshow', {persisted:true}))")
        self.assertNotEqual(p.evaluate('document.activeElement.id'), 'loginPw')
        self.assertEqual(password.input_value(), 'zylz1234')
        self.assertEqual(self.errors, [])

    def test_closed_user_dialogs_cannot_keep_or_receive_editor_focus(self):
        p = self.page
        p.goto(self.url + '/?guide=off')
        p.locator('.handle').click()
        for opener, dialog, field in (
            ('.reset-bar', '[data-reset-modal]', '[data-reset-input]'),
            ('.lab-record', '[data-audio-modal]', '[data-audio-name-input]'),
        ):
            input_box = p.locator(field)
            self.assertFalse(input_box.evaluate('(e) => { e.focus(); return document.activeElement === e }'))
            p.locator(opener).click()
            input_box.tap()
            self.assertEqual(input_box.get_attribute('inputmode'), 'text')
            p.locator(dialog + ' .modal-close').evaluate('(e) => e.click()')
            self.assertFalse(input_box.evaluate('(e) => { e.focus(); return document.activeElement === e }'))
            self.assertEqual(input_box.get_attribute('inputmode'), 'none')
        p.goto(self.url + '/setup?guide=off')
        p.get_by_text('Test WiFi', exact=True).click()
        password = p.locator('#pwInput')
        self.assertEqual(password.get_attribute('inputmode'), 'none')
        password.tap()
        self.assertEqual(password.get_attribute('inputmode'), 'text')
        p.locator('[data-close="pwLayer"]').first.evaluate('(e) => e.click()')
        self.assertFalse(password.evaluate('(e) => { e.focus(); return document.activeElement === e }'))
        self.assertEqual(password.get_attribute('inputmode'), 'none')
        self.assertEqual(self.errors, [])

    def test_android_audio_upload_taps_native_picker_and_keeps_the_selection(self):
        self.page.close()
        self.page = self.browser.new_page(**self.playwright.devices['Pixel 7'])
        self.assert_native_audio_picker()

    def assert_native_audio_picker(self):
        page = self.page
        page.on('pageerror', lambda error: self.errors.append(str(error)))
        page.goto(self.url + '/?guide=off')
        page.locator('.handle').tap()
        page.locator('.lab-upload').tap()
        upload = page.locator('[data-upload-panel]')
        self.assertTrue(upload.is_visible())
        self.assertTrue(page.locator('[data-create-audio]').is_disabled())
        self.assertFalse(upload.locator('[data-audio-browser-help]').is_visible())

        with page.expect_file_chooser() as picker:
            upload.locator('#audio-file-input').tap()
        self.assertEqual(picker.value.element.get_attribute('id'), 'audio-file-input')
        picker.value.set_files({'name': '录音测试.wav', 'mimeType': 'audio/wav', 'buffer': b'RIFF'})
        self.assertEqual(upload.locator('[data-audio-file]').inner_text(), '录音测试.wav')
        self.assertEqual(page.locator('[data-audio-name-input]').input_value(), '录音测试')
        self.assertTrue(page.locator('[data-create-audio]').is_enabled())

        open_pickers = []
        page.on('filechooser', lambda chooser: open_pickers.append(chooser))
        page.locator('[data-create-tab="record"]').tap()
        page.locator('[data-create-tab="upload"]').tap()
        self.assertTrue(page.locator('[data-create-audio]').is_enabled())
        self.assertEqual(open_pickers, [])
        self.assertEqual(self.errors, [])

    def test_android_webview_audio_upload_shows_browser_instructions(self):
        self.page.close()
        device = self.playwright.devices['Pixel 7']
        device['user_agent'] = ('Mozilla/5.0 (Linux; Android 14; Pixel 7; wv) '
                                'AppleWebKit/537.36 Version/4.0 Chrome/120.0.0.0 Mobile Safari/537.36')
        self.page = self.browser.new_page(**device)
        self.page.goto(self.url + '/?guide=off')
        self.page.locator('.handle').tap()
        self.page.locator('.lab-upload').tap()
        help_text = self.page.locator('[data-audio-browser-help]')
        self.assertTrue(help_text.is_visible())
        self.assertIn('Chrome 地址栏打开 192.168.4.1', help_text.inner_text())

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
        p.route('**/api/user/voices', lambda r: r.fulfill(json={
            'ok': True, 'has_key': True, 'voices': [
                {'id': str(i), 'name': '测试音色' + str(i), 'builtin': True}
                for i in range(12)
            ]
        }))
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

    def test_local_detail_stays_open_during_wifi_loss_and_still_allows_volume(self):
        p = self.page
        Portal.wifi_connected = False
        p.goto(self.url + '/?guide=off')
        p.get_by_text('设备暂未联网，可以继续离线玩耍。', exact=True).wait_for()
        self.assertTrue(p.locator('.sheet-main > .primary').is_disabled())
        for _ in range(2):
            with p.expect_response('**/api/status'):
                p.evaluate("window.dispatchEvent(new PageTransitionEvent('pageshow',{persisted:true}))")
        self.assertIn('/?guide=off', p.url)
        with p.expect_response('**/api/user/volume'):
            p.locator('.volume').first.evaluate(
                "(e) => { e.value=35; e.dispatchEvent(new Event('change',{bubbles:true})); }")
        Portal.wifi_connected = True
        p.evaluate("window.dispatchEvent(new PageTransitionEvent('pageshow',{persisted:true}))")
        p.wait_for_function("!document.querySelector('.sheet-main>.primary').disabled")
        self.assertEqual(self.errors, [])

    def test_first_conversation_tutorial_can_be_cancelled_from_page(self):
        p = self.page
        def status(route):
            route.fulfill(json={'build':'wzh-dinosaur', 'state':Portal.state,
                                'conversation_intro':True, 'wifi':{'connected':True},
                                'conversation_configured':True})
        Portal.state = 'connecting'
        p.route('**/api/status', status)
        p.goto(self.url + '/?guide=off')
        p.get_by_text('正在播放首次对话教程，播完就会开始聆听。', exact=True).wait_for()
        with p.expect_response('**/api/user/chat/stop'):
            p.get_by_role('button', name='取消连接', exact=True).first.click()
        self.assertIn('/api/user/chat/stop', Portal.requests)

    def test_conversation_recovery_can_be_cancelled_while_network_is_down(self):
        p = self.page
        p.route('**/api/status', lambda route: route.fulfill(json={
            'build': 'wzh-dinosaur', 'state': 'idle', 'wifi': {'connected': False},
            'conversation_configured': True,
            'conversation_recovering': '/api/user/chat/stop' not in Portal.requests,
        }))
        p.goto(self.url + '/?guide=off')
        p.get_by_text('正在恢复对话连接，可以取消。', exact=True).wait_for()
        with p.expect_response('**/api/user/chat/stop'):
            p.get_by_role('button', name='取消连接', exact=True).first.click()
        p.get_by_text('设备暂未联网，可以继续离线玩耍。', exact=True).wait_for()
        self.assertEqual(self.errors, [])

    def test_no_key_is_explained_before_start_and_record_cancel_reaches_device(self):
        p = self.page
        current = {'id':'xiaozhi-native','name':'原声','current':True,'builtin':True}
        key = {'available':False}
        recording = {'phase':'recording'}
        cancelled = []
        p.route('**/api/user/voices', lambda r: r.fulfill(json={
            'has_key':key['available'], 'voices':[current]}))
        p.route('**/api/user/voices/device-record', lambda r: r.fulfill(status=202, json={'request':7}))
        p.route('**/api/user/voices/device-record/status', lambda r: r.fulfill(json={
            'phase':recording['phase'], 'request':7}))
        def stop(route):
            cancelled.append(route.request.post_data_json['request'])
            recording['phase'] = 'cancelled'
            route.fulfill(status=202, json={'ok':True})
        p.route('**/api/user/voices/device-record/cancel', stop)
        p.goto(self.url + '/?guide=off')
        p.locator('.handle').click()
        p.locator('#voiceServiceNotice').wait_for()
        p.locator('.lab-record').click()
        self.assertFalse(p.locator('[data-audio-modal]').is_visible())
        self.assertEqual(p.locator('.fixed-track .voice-chip').count(), 1)
        key['available'] = True
        p.evaluate("window.dispatchEvent(new PageTransitionEvent('pageshow',{persisted:true}))")
        p.locator('#voiceServiceNotice').wait_for(state='hidden')
        p.locator('.lab-record').click()
        p.locator('[data-record-puck]').click()
        p.get_by_text('设备正在录音，最多 20 秒。', exact=True).wait_for()
        p.wait_for_function("document.querySelector('[data-record-time]').textContent !== '00:00'")
        p.get_by_role('button', name='停止录音', exact=True).click()
        p.locator('[data-voice-operation]').get_by_text('录音已停止。',exact=True).wait_for()
        self.assertEqual(cancelled, [7])

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
        p.goto(self.url + '/developer')
        p.locator('#devPassword').fill('1')
        p.locator('#devLogin').click()
        p.get_by_text('密码不正确').wait_for()
        self.assertTrue(p.locator('#devLogin').is_enabled())
        p.locator('#devPassword').fill(DEVELOPER_PASSWORD)
        p.locator('#devLogin').click()
        p.wait_for_function("document.getElementById('devPhone').dataset.state === 'in'")
        self.assertEqual(Portal.requests.count('/api/developer/login'), 2)
        self.assertEqual(self.errors, [])

    def test_developer_login_blurs_input_and_mobile_fits(self):
        p = self.page
        p.goto(self.url + '/developer')
        p.locator('#devPassword').fill(DEVELOPER_PASSWORD)
        p.locator('#devLogin').click()
        p.wait_for_function("document.getElementById('devPhone').dataset.state === 'in'")
        self.assertNotEqual(p.evaluate('document.activeElement.id'), 'devPassword')
        self.assertLessEqual(p.locator('#devPhone').bounding_box()['width'], 390)
        self.assertEqual(self.errors, [])

    def test_factory_hub_contacts_server_only_after_a_manual_action(self):
        page = self.page
        posts = []
        running_reads = []

        def check(route):
            posts.append(route.request.url)
            running_reads.extend([True, False])
            route.fulfill(status=202, json={'ok': True})

        def status(route):
            running = running_reads.pop(0) if running_reads else False
            route.fulfill(json={'hub': {'available': True, 'activated': bool(posts),
                                        'job_running': running,
                                        'job_message': '检查完成' if posts and not running else ''}})

        def diff(route):
            route.fulfill(json={'ready': bool(posts), 'items': [
                {'name': 'call', 'state': 'new'}, {'name': 'recording', 'state': 'local'},
            ] if posts else []})

        page.route('**/api/hub/status', status)
        page.route('**/api/hub/diff', diff)
        page.route('**/api/hub/check', check)
        page.route('**/api/hub/update', check)
        page.goto(self.url + '/developer')
        page.locator('#devPassword').fill(DEVELOPER_PASSWORD)
        page.locator('#devLogin').click()
        page.locator('[data-tab="hub"]').click()
        page.wait_for_function("!document.getElementById('hubCheck').disabled")
        self.assertEqual(posts, [])
        self.assertTrue(page.locator('#hubUpdate').is_disabled())
        page.locator('#hubCheck').click()
        page.get_by_text('处理中', exact=True).wait_for()
        self.assertTrue(page.locator('#hubCheck').is_disabled())
        self.assertTrue(page.locator('#hubUpdate').is_disabled())
        page.get_by_text('检查完成', exact=True).wait_for(timeout=4500)
        page.locator('[data-hub-item="call"]').wait_for()
        self.assertTrue(page.locator('[data-hub-item="recording"]').is_disabled())
        self.assertTrue(page.locator('#hubUpdate').is_enabled())
        self.assertEqual(posts, [self.url + '/api/hub/check'])
        self.assertEqual(self.errors, [])

    def test_developer_voice_audition_waits_for_terminal_status(self):
        page = self.page
        phases = ['queued', 'complete']
        posts = []

        def audition(route):
            posts.append(route.request.post_data_json)
            route.fulfill(status=202, json={'ok': True})

        page.route('**/api/developer/voice-config', audition)
        page.route('**/api/developer/voice-preview/status',
                   lambda route: route.fulfill(json={'phase': phases.pop(0)}))
        page.goto(self.url + '/developer')
        page.locator('#devPassword').fill(DEVELOPER_PASSWORD)
        page.locator('#devLogin').click()
        page.wait_for_function("document.getElementById('devPhone').dataset.state === 'in'")
        page.locator('[data-tab="voice"]').click()
        page.locator('#devVoicePreview').click()
        page.get_by_text('正在准备试听', exact=True).wait_for()
        self.assertTrue(page.locator('#devVoiceAudition').is_disabled())
        page.get_by_text('试听结束', exact=True).wait_for(timeout=4000)
        self.assertEqual(posts, [{'kind': 'ali_preview', 'value': ''}])
        self.assertTrue(page.locator('#devVoiceAudition').is_enabled())
        self.assertEqual(self.errors, [])

    def test_developer_runtime_and_network_show_measured_diagnostics(self):
        page = self.page
        payload = {
            'build': 'wzh-dinosaur', 'state': 'idle', 'conversation': False, 'uptime_s': 68,
            'version': '2.4.1', 'idf_version': 'v6.0.2', 'chip': 'esp32s3',
            'app_partition': 'ota_0', 'app_partition_bytes': 4194304,
            'nvs_used_entries': 14, 'nvs_total_entries': 126,
            'heap_internal': 32768, 'heap_internal_min': 28672, 'heap_internal_largest': 8192,
            'heap_psram': 6291456, 'heap_psram_min': 4718592, 'heap_psram_largest': 4194304,
            'servo_powered': True, 'servo_faults': 4, 'transport': 'cellular',
            'network_available': True, 'cellular_server_connected': False,
            'wifi': {'connected': False, 'rssi': 0, 'ap_ssid': 'Dinosaur-ABCD',
                     'ap_ip': '192.168.4.1', 'ap_port': 8080, 'ap_security': 'open', 'ap_clients': 2},
            'battery': {'available': True, 'level': 61, 'rest_ready': True, 'voltage_mv': 3816,
                        'live_voltage_mv': 3740, 'protection_voltage_mv': 3700, 'sample_valid': True,
                        'under_load': True, 'rest_age_ms': 2200},
            'audio': {'input_enabled': True, 'capture_running': False, 'wake_running': True,
                      'playback_idle': False, 'buffered_ms': 120},
        }
        page.route('**/api/developer/status', lambda route: route.fulfill(json=payload))
        page.route('**/api/status', lambda route: route.fulfill(json=payload))
        requests = []
        page.on('request', lambda request: requests.append(urlsplit(request.url).path))
        page.goto(self.url + '/developer')
        page.locator('#devPassword').fill(DEVELOPER_PASSWORD)
        page.locator('#devLogin').click()
        page.get_by_text('4G 已联网', exact=True).wait_for()
        self.assertEqual(page.locator('#networkDetail').text_content(), '对话服务未连接')
        self.assertEqual(page.locator('#batteryRest').text_content(), '3816 mV')
        self.assertEqual(page.locator('#batteryVoltages').text_content(), '3740 mV / 3700 mV')
        payload['battery'].update(rest_ready=False, rest_age_ms=5200, live_voltage_mv=3500)
        page.wait_for_function("document.getElementById('batteryVoltages').textContent === '3500 mV / 3700 mV'")
        self.assertEqual(page.locator('#batteryLevel').text_content(), '61%')
        self.assertEqual(page.locator('#batteryRest').text_content(), '3816 mV')
        self.assertEqual(page.locator('#batterySample').text_content(), '负载中 / 5 秒前')
        self.assertEqual(page.locator('#servoFaults').text_content(), '0x4')
        self.assertEqual(page.locator('#playbackState').text_content(), '活跃 / 120 ms')
        self.assertNotIn('/api/hub/status', requests)
        self.assertNotIn('/api/developer/ota', requests)
        page.locator('[data-tab="network"]').click()
        self.assertEqual(page.locator('#netRssi').text_content(), '--')
        self.assertTrue(page.locator('#netServerRow').is_visible())
        self.assertEqual(page.locator('#netDevAddress').text_content(), '192.168.4.1:8080')
        self.assertEqual(page.locator('#netApClients').text_content(), '开放 / 2 个')
        payload['cellular_server_connected'] = True
        page.locator('#netDiag').click()
        page.wait_for_function("document.getElementById('netServer').textContent === '已连接'")
        payload.update(transport='probing', network_available=False)
        page.locator('#netDiag').click()
        page.get_by_text('正在选择联网通道', exact=True).last.wait_for()
        self.assertTrue(page.locator('#netServerRow').is_hidden())
        page.locator('[data-tab="system"]').click()
        page.get_by_text('v6.0.2', exact=True).wait_for()
        self.assertTrue(page.get_by_text('ota_0 · 4.0 MiB', exact=True).is_visible())
        self.assertTrue(page.get_by_text('32.0 KiB / 28.0 KiB', exact=True).is_visible())
        self.assertTrue(page.get_by_text('6.0 MiB / 4.5 MiB', exact=True).is_visible())
        self.assertTrue(page.get_by_text('4.0 MiB', exact=True).is_visible())
        self.assertEqual(self.errors, [])

    def test_developer_touch_reads_four_live_channels_only_on_status_tab(self):
        page = self.page
        page.set_viewport_size({'width': 320, 'height': 568})
        payload = {'build': 'wzh-crawlpanda', 'state': 'idle',
                   'ali_voice_id': 'xiaozhi-native', 'ali_previous_voice_id': 'xiaozhi-native',
                   'touch': {'available': True, 'valid': False, 'code': -1, 'raw_code': 5, 'voltage_mv': 950}}
        live_reads = []

        def status(route):
            if 'x-developer-token' in route.request.headers and route.request.url.endswith('/api/status'):
                live_reads.append(route.request.url)
            route.fulfill(json=payload)

        def wait_summary(text):
            page.wait_for_function('(text) => document.querySelector("#touchSummary").textContent === text', arg=text)

        page.route('**/api/status', status)
        page.route('**/api/developer/status', status)
        page.goto(self.url + '/developer')
        page.locator('#devPassword').fill(DEVELOPER_PASSWORD)
        page.locator('#devLogin').click()
        wait_summary('等待稳定')
        self.assertEqual(page.locator('#devVoiceName').text_content(), '原声')
        self.assertEqual(page.locator('#devPrevVoice').text_content(), '原声')
        self.assertEqual(page.locator('[data-touch-sw][data-state="pending"]').count(), 4)
        self.assertEqual(page.locator('[data-touch-location]').all_text_contents(), ['头', '肚子', '背部前侧', '背部后侧'])

        payload['touch'].update(valid=True, code=5)
        wait_summary('2 路触发')
        self.assertTrue(live_reads)
        self.assertEqual(page.locator('[data-touch-sw] [data-touch-state]').all_text_contents(),
                         ['已触摸', '已松开', '已触摸', '已松开'])
        self.assertEqual(page.locator('#touchDiagnostics').text_content(), 'IO5 950 mV · 稳定 0x05 · 原始 0x05')

        payload['build'] = 'wzh-tailredpanda'
        payload['touch'].update(code=0, raw_code=0, voltage_mv=90)
        wait_summary('全部松开')
        self.assertEqual(page.locator('[data-touch-location]').all_text_contents(), ['头', '背部', '肚子', '尾巴'])
        self.assertEqual(page.locator('[data-touch-sw][data-state="released"]').count(), 4)

        def set_isolation(route):
            self.assertIn('x-developer-token', route.request.headers)
            payload['touch']['sw4_isolated'] = route.request.post_data_json['isolated']
            route.fulfill(json={'isolated': payload['touch']['sw4_isolated']})

        page.route('**/api/developer/touch/sw4', set_isolation)
        payload['touch'].update(sw4_isolated=True, raw_code=8, voltage_mv=1634)
        page.get_by_text('已隔离', exact=True).wait_for()
        self.assertIn('原始 0x08', page.locator('#touchDiagnostics').text_content())
        page.get_by_role('button', name='恢复 SW4 触摸').click()
        page.get_by_text('SW4 触摸已恢复', exact=True).wait_for()
        self.assertFalse(payload['touch']['sw4_isolated'])
        page.get_by_role('button', name='隔离 SW4 触摸').click()
        page.get_by_text('SW4 已隔离', exact=True).wait_for()
        self.assertTrue(payload['touch']['sw4_isolated'])

        page.locator('[data-tab="system"]').click()
        reads_before = len(live_reads)
        payload['build'] = 'wzh-dinosaur'
        payload.pop('touch')
        page.wait_for_timeout(1200)
        self.assertEqual(len(live_reads), reads_before)
        page.locator('[data-tab="status"]').click()
        wait_summary('不可用')
        self.assertEqual(page.locator('[data-touch-location]').all_text_contents(),
                         ['头', '胸前', '背部靠近脖子', '背部靠近尾巴'])
        self.assertEqual(page.locator('[data-touch-sw] [data-touch-state]').all_text_contents(), ['不可用'] * 4)
        self.assertEqual(self.errors, [])
