#!/usr/bin/env python3
"""Pack WZH web assets with a deterministic, content-derived cache revision."""
import gzip
import hashlib
import json
import pathlib
import re
import sys

root = pathlib.Path(sys.argv[1])
out = pathlib.Path(sys.argv[2])
header = pathlib.Path(sys.argv[3])
# Optional product id (crawlPanda / dinosaur / redpanda). The three products share
# one page set and swap the toy image at runtime via product.js, so a given build
# only needs its own toy photos. When omitted (tests, tooling), every image ships.
product = sys.argv[4] if len(sys.argv) > 4 else ''
TOY_PREFIXES = {'crawlPanda': ('crawlPanda_',), 'dinosaur': ('dinosaur_',),
                'redpanda': ('tailRedPanda_',)}
unused = {'wifi.html', 'wifi.js', 'wifi.css', 'dinosaur_face.webp', 'dinosaur_head.webp',
          'panda-face.webp', 'panda-head.webp', 'tailRedPanda_face.webp', 'tailRedPanda_head.webp'}
if product and product not in TOY_PREFIXES:
    raise SystemExit(f'unknown product id: {product!r}')


def keep(path: pathlib.Path) -> bool:
    if path.name in unused:
        return False
    if not product or path.parent.name != 'assets' or path.suffix != '.webp':
        return True
    # Shared brand assets (not tied to any toy) always ship.
    if not any(path.name.startswith(p) for prefixes in TOY_PREFIXES.values() for p in prefixes):
        return True
    # Each firmware build serves one product only (product.js syncs the toy to
    # /api/status and HTML placeholders are inline), so ship just its own photos.
    return path.name.startswith(TOY_PREFIXES[product])


paths = sorted(p for p in root.rglob('*') if p.is_file() and keep(p))
assets = {path.relative_to(root).as_posix(): path.read_bytes() for path in paths}
hash_state = hashlib.sha256()
# A packer change can alter the served bytes even when the source assets do not.
hash_state.update(pathlib.Path(__file__).read_bytes())
hash_state.update(product.encode('ascii'))
for name, data in assets.items():
    encoded_name = name.encode('utf-8')
    hash_state.update(len(encoded_name).to_bytes(4, 'big'))
    hash_state.update(encoded_name)
    hash_state.update(len(data).to_bytes(8, 'big'))
    hash_state.update(data)
revision = hash_state.hexdigest()[:16]
# These are local URLs in HTML, CSS and JS; preserve fragments and unrelated queries.
asset_url = re.compile(r'''(?P<path>(?:assets|css|js)/[^\s"'<>?#)]+\.(?:css|js|webp))(?:\?v=[A-Za-z0-9._-]+)?''')
# Pack the scripts used only by the detail page into a single transfer. Other
# pages can still reuse the shared guide, theme and editor files from login.
detail_scripts = ('js/device-detail.js', 'js/index-app.js', 'js/index-view.js')
detail_tags = '\n'.join(f'<script defer src="{name}"></script>' for name in detail_scripts).encode()
if detail_tags in assets.get('index.html', b''):
    assets['index.html'] = assets['index.html'].replace(
        detail_tags, b'<script defer src="js/detail.js"></script>', 1)
    assets['js/detail.js'] = b'\n;\n'.join(assets.pop(name) for name in detail_scripts)

entries = []
blob = bytearray()
for name, data in sorted(assets.items()):
    suffix = pathlib.PurePosixPath(name).suffix
    compressed = suffix in ('.html', '.css', '.js')
    if compressed:
        source = data.decode('utf-8')
        if product and suffix == '.html':
            source = source.replace('<html ', f'<html data-build-product="{product}" ', 1)
        source = asset_url.sub(lambda match: match.group('path') + '?v=' + revision, source)
        data = gzip.compress(source.encode('utf-8'), compresslevel=9, mtime=0)
    entries.append((name, len(blob), len(data), compressed))
    blob.extend(data)
out.write_bytes(blob)
header.write_text('#define WZH_UI_REVISION "' + revision + '"\n' +
                  '\n'.join('    {' + json.dumps(n, ensure_ascii=True) + f', {o}, {s}, ' +
                            ('true' if c else 'false') + '},' for n, o, s, c in entries) + '\n')
