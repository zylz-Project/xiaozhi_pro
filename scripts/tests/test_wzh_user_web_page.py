"""Browser smoke tests for the embedded, three-product user portal."""
import json
import io
import shutil
import threading
import unittest
import wave
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

try:
    from playwright.sync_api import sync_playwright
except ImportError:
    sync_playwright = None

ROOT = Path(__file__).resolve().parents[2]
PAGE = (ROOT / 'main/assets/wzh_user_web.html').read_bytes()
DEVELOPER_PAGE = (ROOT / 'main/assets/wzh_console.html').read_bytes()

class Handler(BaseHTTPRequestHandler):
    product = 'wzh-crawlpanda'
    requests = []
    uploaded_bytes = 0
    record_phase = "idle"
    record_name = ""
    def log_message(self, *args):
        pass
    def json(self, obj):
        body = json.dumps(obj).encode()
        self.send_response(200); self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body))); self.end_headers(); self.wfile.write(body)
    def do_GET(self):
        self.requests.append((self.command, self.path))
        if self.path == '/console':
            self.send_response(200); self.send_header('Content-Type','text/html; charset=utf-8')
            self.send_header('Content-Length',str(len(DEVELOPER_PAGE))); self.end_headers(); self.wfile.write(DEVELOPER_PAGE)
            return
        if self.path == '/api/status':
            return self.json({'ok':True,'build':self.product,'version':'test','state':'idle',
                'wifi':{'connected':True,'ssid':'Home','rssi':-40,'ip':'192.168.1.10'},
                'battery':{'available':True,'percent':70,'charging':False},
                'temperature_c':32,'audio':{'volume':40},
                'capabilities':{'voice':self.product in ('wzh-crawlpanda','wzh-tailredpanda','wzh-dinosaur')}})
        if self.path == '/scan':
            return self.json({'ok':True,'aps':[{'ssid':'<img src=x onerror=alert(1)>','rssi':-40,'auth':3}]})
        if self.path == '/api/user/voices/device-record/status':
            return self.json({'ok': True, 'phase': Handler.record_phase, 'failure': ''})
        if self.path == '/api/user/voices/clone/status':
            return self.json({'ok':True,'phase':'done'})
        if self.path == '/api/user/voices':
            return self.json({'ok':True,'has_key':True,'voice_management_loaded':True,
                              'voice_management_busy':False,'voices':[]})
        self.send_response(200); self.send_header('Content-Type','text/html; charset=utf-8')
        self.send_header('Content-Length',str(len(PAGE))); self.end_headers(); self.wfile.write(PAGE)
    def do_POST(self):
        self.requests.append((self.command,self.path))
        body = self.rfile.read(int(self.headers.get('Content-Length','0')))
        if self.path == '/api/user/voices/clone':
            Handler.uploaded_bytes = len(body)
        if self.path == '/api/user/voices/device-record':
            Handler.record_name = json.loads(body)['name']
            Handler.record_phase = 'recording'
        self.json({'ok':True,'volume':65})

@unittest.skipUnless(sync_playwright and shutil.which('google-chrome'), 'Chrome and Playwright required')
class UserPortalPageTest(unittest.TestCase):
    def test_setup_status_and_product_capabilities(self):
        server=ThreadingHTTPServer(('127.0.0.1',0), Handler)
        thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
        try:
            with sync_playwright() as p:
                browser=p.chromium.launch(executable_path=shutil.which('google-chrome'),headless=True,args=['--no-sandbox'])
                page=browser.new_page(viewport={'width':390,'height':844})
                errors=[];page.on('pageerror',lambda error:errors.append(str(error)))
                page.goto(f'http://127.0.0.1:{server.server_port}/setup')
                page.get_by_text('<img src=x onerror=alert(1)>').wait_for()
                self.assertEqual(page.locator('#networks img').count(),0)
                page.get_by_text('<img src=x onerror=alert(1)>').click()
                self.assertEqual(page.locator('#ssid').input_value(),'<img src=x onerror=alert(1)>')
                Handler.product='wzh-dinosaur';page.goto(f'http://127.0.0.1:{server.server_port}/')
                page.get_by_text('Dino',exact=True).wait_for()
                self.assertTrue(page.locator('#voices').is_visible())
                page.locator('#volume').fill('65');page.locator('#volume').dispatch_event('change')
                page.wait_for_timeout(200)
                self.assertIn(('POST','/api/user/volume'),Handler.requests)
                Handler.product='wzh-tailredpanda';page.goto(f'http://127.0.0.1:{server.server_port}/')
                page.get_by_text('RedPanda',exact=True).wait_for()
                self.assertTrue(page.locator('#voices').is_visible())
                Handler.product='wzh-crawlpanda';page.goto(f'http://127.0.0.1:{server.server_port}/')
                page.get_by_text('CrawlPanda',exact=True).wait_for()
                self.assertTrue(page.locator('#voices').is_visible())
                page.wait_for_timeout(300)
                self.assertIn(('GET','/api/user/voices'),Handler.requests)
                self.assertEqual(errors,[])
                browser.close()
        finally:
            server.shutdown();server.server_close();thread.join(timeout=3)
            Handler.product='wzh-crawlpanda';Handler.requests.clear()
            Handler.uploaded_bytes=0

    def test_device_recording_for_all_products(self):
        Handler.requests.clear()
        server=ThreadingHTTPServer(('127.0.0.1',0), Handler)
        thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
        try:
            with sync_playwright() as p:
                browser=p.chromium.launch(executable_path=shutil.which('google-chrome'),headless=True,args=['--no-sandbox'])
                page=browser.new_page()
                for product in ('wzh-dinosaur','wzh-crawlpanda','wzh-tailredpanda'):
                    Handler.product=product;Handler.record_phase='idle'
                    page.goto(f'http://127.0.0.1:{server.server_port}/')
                    page.locator('#voiceName').fill('我的声音')
                    page.locator('#voiceDeviceRecord').click()
                    page.get_by_text('设备正在录音，请清晰说话。').wait_for(timeout=5000)
                    self.assertEqual(Handler.record_name,'我的声音')
                    self.assertTrue(page.locator('#voiceDeviceRecord').is_disabled())
                    self.assertTrue(page.locator('#cloneVoice').is_disabled())
                    Handler.record_phase='success'
                    page.get_by_text('音色创建成功。').wait_for(timeout=5000)
                    self.assertTrue(page.locator('#voiceFileLabel').is_visible())
                self.assertEqual(Handler.requests.count(('POST','/api/user/voices/device-record')),3)
                browser.close()
        finally:
            server.shutdown();server.server_close();thread.join(timeout=3)
            Handler.product='wzh-crawlpanda';Handler.record_phase='idle';Handler.requests.clear()

    def test_reconnect_and_failed_device_recording(self):
        Handler.record_phase='recording'
        server=ThreadingHTTPServer(('127.0.0.1',0), Handler)
        thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
        try:
            with sync_playwright() as p:
                browser=p.chromium.launch(executable_path=shutil.which('google-chrome'),headless=True,args=['--no-sandbox'])
                page=browser.new_page()
                page.goto(f'http://127.0.0.1:{server.server_port}/')
                page.get_by_text('设备正在录音，请清晰说话。').wait_for(timeout=5000)
                self.assertTrue(page.locator('#voiceDeviceRecord').is_disabled())
                Handler.record_phase='failed'
                page.get_by_text('创建失败，请检查设备和网络后重试。').wait_for(timeout=5000)
                self.assertTrue(page.locator('#voiceDeviceRecord').is_enabled())
                Handler.record_phase='success'
                page.reload()
                page.get_by_text('上次设备录音已创建成功，可在音色列表查看。').wait_for(timeout=5000)
                browser.close()
        finally:
            server.shutdown();server.server_close();thread.join(timeout=3)
            Handler.record_phase='idle';Handler.requests.clear()

    def test_ios_and_android_recording_paths(self):
        Handler.requests.clear();Handler.uploaded_bytes=0
        server=ThreadingHTTPServer(('127.0.0.1',0), Handler)
        thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
        try:
            with sync_playwright() as p:
                browser=p.chromium.launch(executable_path=shutil.which('google-chrome'),headless=True,args=['--no-sandbox'])
                iphone=browser.new_context(user_agent='Mozilla/5.0 (iPhone; CPU iPhone OS 17_0 like Mac OS X) AppleWebKit/605.1.15 Version/17.0 Mobile Safari/604.1',viewport={'width':390,'height':844})
                page=iphone.new_page();page.add_init_script("Object.defineProperty(window,'isSecureContext',{value:false})");errors=[]
                page.on('pageerror',lambda error:errors.append(str(error)))
                page.goto(f'http://127.0.0.1:{server.server_port}/')
                page.locator('#voiceName').wait_for(state='visible')
                self.assertTrue(page.locator('#voiceDeviceRecord').is_visible())
                self.assertTrue(page.locator('#voiceFileLabel').is_visible())
                self.assertIsNone(page.locator('#voiceFile').get_attribute('accept'))
                self.assertEqual(page.locator('#voiceRecordButton').count(),0)
                # iOS file picking has no restrictive audio/* filter. The file
                # reader fallback still produces a 16 kHz mono PCM upload.
                audio=io.BytesIO()
                with wave.open(audio,'wb') as wav:
                    wav.setnchannels(1);wav.setsampwidth(2);wav.setframerate(44100)
                    wav.writeframes(b'\x00\x00'*(44100*6))
                page.evaluate("Object.defineProperty(File.prototype,'arrayBuffer',{value:undefined,configurable:true})")
                page.evaluate("window.OfflineAudioContext=undefined;window.webkitOfflineAudioContext=undefined")
                page.locator('#voiceFile').set_input_files({'name':'memo.wav','mimeType':'audio/wav','buffer':audio.getvalue()})
                page.locator('#voiceName').fill('文件声音')
                page.locator('#cloneVoice').click()
                page.wait_for_function("document.getElementById('voiceMessage').textContent === '创建成功' && !document.getElementById('cloneVoice').disabled",timeout=10000)
                self.assertEqual(Handler.uploaded_bytes,16000*6*2)
                page.goto(f'http://127.0.0.1:{server.server_port}/console')
                page.locator('#voiceFile').wait_for(state='attached')
                self.assertIsNone(page.locator('#voiceFile').get_attribute('accept'))
                self.assertIsNone(page.locator('#voiceFile').get_attribute('capture'))
                self.assertEqual(page.locator('#voiceDeviceRecord').count(),1)
                self.assertEqual(page.get_by_text('用设备麦克风录音').count(),1)
                iphone.close()
                ipad=browser.new_context(user_agent='Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15) AppleWebKit/605.1.15 Version/17.0 Safari/605.1.15')
                page=ipad.new_page()
                page.add_init_script("Object.defineProperty(navigator,'platform',{get:()=> 'MacIntel'});Object.defineProperty(navigator,'maxTouchPoints',{get:()=>5})")
                page.goto(f'http://127.0.0.1:{server.server_port}/')
                page.locator('#voiceName').wait_for(state='visible')
                self.assertTrue(page.locator('#voiceDeviceRecord').is_visible())
                self.assertTrue(page.locator('#voiceFileLabel').is_visible())
                self.assertIsNone(page.locator('#voiceFile').get_attribute('accept'))
                ipad.close()
                android=browser.new_context(user_agent='Mozilla/5.0 (Linux; Android 14; Pixel 8) AppleWebKit/537.36 Chrome/127.0 Mobile Safari/537.36')
                page=android.new_page();page.add_init_script("Object.defineProperty(window,'isSecureContext',{value:false})");page.goto(f'http://127.0.0.1:{server.server_port}/')
                page.locator('#voiceName').wait_for(state='visible')
                self.assertTrue(page.locator('#voiceDeviceRecord').is_visible())
                self.assertTrue(page.locator('#voiceFileLabel').is_visible())
                self.assertEqual(page.locator('#voiceFile').get_attribute('accept'),'audio/*')
                android.close();browser.close()
        finally:
            server.shutdown();server.server_close();thread.join(timeout=3)
            Handler.product='wzh-crawlpanda';Handler.requests.clear()
            Handler.uploaded_bytes=0
