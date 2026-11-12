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
unused = {'wifi.html', 'wifi.js', 'wifi.css', 'dinosaur_face.webp', 'dinosaur_head.webp',
          'panda-face.webp', 'panda-head.webp', 'tailRedPanda_face.webp', 'tailRedPanda_head.webp'}
paths = sorted(p for p in root.rglob('*') if p.is_file() and p.name not in unused)
hash_state = hashlib.sha256()
# A packer change can alter the served bytes even when the source assets do not.
hash_state.update(pathlib.Path(__file__).read_bytes())
for path in paths:
    name = path.relative_to(root).as_posix().encode('utf-8')
    data = path.read_bytes()
    hash_state.update(len(name).to_bytes(4, 'big'))
    hash_state.update(name)
    hash_state.update(len(data).to_bytes(8, 'big'))
    hash_state.update(data)
revision = hash_state.hexdigest()[:16]
# These are local URLs in HTML, CSS and JS; preserve fragments and unrelated queries.
asset_url = re.compile(r'''(?P<path>(?:assets|css|js)/[^\s"'<>?#)]+\.(?:css|js|webp))(?:\?v=[A-Za-z0-9._-]+)?''')
entries = []
blob = bytearray()
for path in paths:
    name = path.relative_to(root).as_posix()
    data = path.read_bytes()
    compressed = path.suffix in ('.html', '.css', '.js')
    if compressed:
        source = data.decode('utf-8')
        source = asset_url.sub(lambda match: match.group('path') + '?v=' + revision, source)
        data = gzip.compress(source.encode('utf-8'), compresslevel=9, mtime=0)
    entries.append((name, len(blob), len(data), compressed))
    blob.extend(data)
out.write_bytes(blob)
header.write_text('#define WZH_UI_REVISION "' + revision + '"\n' +
                  '\n'.join('    {' + json.dumps(n, ensure_ascii=True) + f', {o}, {s}, ' +
                            ('true' if c else 'false') + '},' for n, o, s, c in entries) + '\n')
