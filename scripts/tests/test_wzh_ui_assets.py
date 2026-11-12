"""Verify asset versions and firmware cache responses across UI revisions."""
import gzip
import json
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PACKER = ROOT / 'scripts/pack_wzh_ui.py'


def pack(source, work):
    binary, manifest = work / 'ui.bin', work / 'wzh_ui_manifest.inc'
    subprocess.run(['python3', str(PACKER), str(source), str(binary), str(manifest)], check=True)
    lines = manifest.read_text()
    revision = re.search(r'WZH_UI_REVISION "([0-9a-f]+)"', lines).group(1)
    contents = {}
    blob = binary.read_bytes()
    for m in re.finditer(r'\{(".*?"), (\d+), (\d+), (true|false)\}', lines):
        name = json.loads(m[1]); start = int(m[2]); data = blob[start:start + int(m[3])]
        contents[name] = gzip.decompress(data) if m[4] == 'true' else data
    return revision, contents, blob, lines


class AssetPackTest(unittest.TestCase):
    def test_revision_changes_with_content_and_versions_nested_references(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp); source = base / 'source'; (source / 'css').mkdir(parents=True)
            (source / 'js').mkdir(); (source / 'assets').mkdir()
            (source / 'index.html').write_text('<link href="css/a.css"><script src="js/a.js?v=2"></script>')
            (source / 'css/a.css').write_text('p{background:url(assets/a.webp)}')
            (source / 'js/a.js').write_text("let image='assets/a.webp';")
            (source / 'assets/a.webp').write_bytes(b'first')
            v1, files, b1, h1 = pack(source, base)
            self.assertIn(('css/a.css?v=' + v1).encode(), files['index.html'])
            self.assertIn(('js/a.js?v=' + v1).encode(), files['index.html'])
            self.assertIn(('assets/a.webp?v=' + v1).encode(), files['js/a.js'])
            self.assertIn(('assets/a.webp?v=' + v1).encode(), files['css/a.css'])
            self.assertEqual((v1, b1, h1), (lambda r: (r[0], r[2], r[3]))(pack(source, base)))
            (source / 'assets/a.webp').write_bytes(b'second')
            v2, newer, _, _ = pack(source, base)
            self.assertNotEqual(v1, v2)
            self.assertIn(('css/a.css?v=' + v2).encode(), newer['index.html'])

    @unittest.skipUnless(shutil.which('g++'), 'g++ required')
    def test_device_only_caches_matching_versioned_static_assets(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            (work / 'wzh_ui_manifest.inc').write_text('#define WZH_UI_REVISION "abcd"\n'
                '{"css/a.css", 0, 2, true},\n{"index.html", 2, 1, true},\n')
            (work / 'esp_http_server.h').write_text(r'''
#pragma once
#include <stddef.h>
#include <string>
#include <map>
typedef int esp_err_t;
#define ESP_OK 0
#define HTTPD_404_NOT_FOUND 404
struct httpd_req_t { std::string query; std::map<std::string,std::string> headers; };
int httpd_req_get_url_query_str(httpd_req_t*, char*, size_t);
int httpd_query_key_value(const char*, const char*, char*, size_t);
int httpd_resp_set_hdr(httpd_req_t*, const char*, const char*);
int httpd_resp_set_type(httpd_req_t*, const char*);
int httpd_resp_send(httpd_req_t*, const char*, size_t);
int httpd_resp_send_err(httpd_req_t*, int, const char*);
''')
            (work / 'test.cc').write_text(r'''
#include <cassert>
#include <cstring>
#include <string>
#include "esp_http_server.h"
extern const unsigned char pack[] asm("_binary_wzh_ui_pack_bin_start");
const unsigned char pack[] = {1, 2, 3};
int httpd_req_get_url_query_str(httpd_req_t* r,char* p,size_t n) { if(r->query.empty())return -1; std::snprintf(p,n,"%s",r->query.c_str());return 0; }
int httpd_query_key_value(const char* q,const char* k,char* out,size_t n) { std::string s(q),key=std::string(k)+"="; auto pos=s.find(key); if(pos==std::string::npos)return -1; auto v=s.substr(pos+key.size()); v=v.substr(0,v.find('&')); std::snprintf(out,n,"%s",v.c_str());return 0; }
int httpd_resp_set_hdr(httpd_req_t* r,const char* k,const char* v){r->headers[k]=v;return 0;}
int httpd_resp_set_type(httpd_req_t*,const char*){return 0;}
int httpd_resp_send(httpd_req_t*,const char*,size_t){return 0;}
int httpd_resp_send_err(httpd_req_t*,int,const char*){return 0;}
#include "wzh_web_assets.cc"
int main(){
 httpd_req_t a; a.query="v=abcd"; wzh::SendWebAsset(&a,"css/a.css"); assert(a.headers["Cache-Control"]=="public, max-age=31536000, immutable"); assert(a.headers["Content-Encoding"]=="gzip");
 httpd_req_t b; b.query="v=old"; wzh::SendWebAsset(&b,"css/a.css"); assert(b.headers["Cache-Control"]=="no-store");
 httpd_req_t c; wzh::SendWebAsset(&c,"css/a.css"); assert(c.headers["Cache-Control"]=="no-store");
 httpd_req_t d; d.query="v=abcd"; wzh::SendWebAsset(&d,"index.html"); assert(d.headers["Cache-Control"]=="no-store");
}
''')
            subprocess.run(['g++', '-std=c++17', '-I', str(work), '-I',
                            str(ROOT / 'main/boards/common/wzh'), str(work / 'test.cc'),
                            '-o', str(work / 'test')], check=True)
            subprocess.run([str(work / 'test')], check=True)
