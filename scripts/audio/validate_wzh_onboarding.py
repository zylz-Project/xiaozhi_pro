#!/usr/bin/env python3
"""Validate the separately recorded, product-specific onboarding prompts."""
import hashlib
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2] / 'main/assets'
KEYS = {'intro', 'touch', 'motion', 'cmd1', 'net_invite', 'wake', 'ap', 'page',
        'connected', 'ability', 'interrupt', 'clone', 'farewell', 'idle_miss',
        'adv_idle_seen', 'adv_wifi_waiting'}


def validate(profile):
    root = ROOT / 'wzh_onboarding_voices'
    manifest = json.loads((root / 'manifest.json').read_text(encoding='utf-8'))
    if manifest.get('schema') != 1 or profile not in ('an', 'xing', 'long'):
        raise ValueError('invalid onboarding manifest or product')
    files = manifest['products'][profile]
    directory = root / profile
    if set(files) != KEYS or {path.stem for path in directory.glob('*.ogg')} != KEYS:
        raise ValueError('incomplete onboarding audio pack')
    for key in sorted(KEYS):
        data = (directory / (key + '.ogg')).read_bytes()
        if not data.startswith(b'OggS') or hashlib.sha256(data).hexdigest() != files[key]['sha256']:
            raise ValueError('invalid onboarding audio: ' + key)
        if not files[key].get('text'):
            raise ValueError('missing onboarding transcript: ' + key)
        if not (ROOT / 'common' / (key + '.ogg')).is_file():
            raise ValueError('missing common sound symbol: ' + key)


if __name__ == '__main__':
    try:
        validate(sys.argv[1])
    except (IndexError, KeyError, OSError, ValueError) as error:
        sys.exit('Onboarding audio rejected: ' + str(error))
