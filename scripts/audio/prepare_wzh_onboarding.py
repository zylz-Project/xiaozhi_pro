#!/usr/bin/env python3
"""Convert supplied per-product recordings using the system-prompt pipeline."""
import argparse
import json
import shutil
import tempfile
from pathlib import Path

from prepare_wzh_system_prompts import SPEECH_TEMPO, convert, inspect_audio, sha256
from validate_wzh_onboarding import KEYS

ROOT = Path(__file__).resolve().parents[2] / 'main/assets'
PRODUCTS = ('an', 'xing', 'long')


def prepare(source):
    root = ROOT / 'wzh_onboarding_voices'
    common = ROOT / 'common'
    if root.exists() or any((common / (key + '.ogg')).exists() for key in KEYS):
        raise ValueError('onboarding audio already exists; refuse to overwrite it')
    entries = {entry['id']: entry for entry in json.loads(
        (source / '05-数据/audio_placement.json').read_text(encoding='utf-8'))}
    with tempfile.TemporaryDirectory(prefix='wzh-onboarding-') as directory:
        stage = Path(directory)
        manifest = {'schema': 1, 'tempo': SPEECH_TEMPO, 'products': {}}
        for product in PRODUCTS:
            folder = stage / product
            folder.mkdir()
            manifest['products'][product] = {}
            for key in sorted(KEYS):
                recording = source / 'audio' / product / (key + '.mp3')
                output = folder / (key + '.ogg')
                level = convert(recording, output, SPEECH_TEMPO)
                duration = inspect_audio(output, 30)
                manifest['products'][product][key] = {
                    'text': entries[key]['text'][product], 'sha256': sha256(output),
                    'source_sha256': sha256(recording), 'duration_seconds': round(duration, 3),
                    'rms_dbfs': round(level, 2),
                }
        if root.exists() or any((common / (key + '.ogg')).exists() for key in KEYS):
            raise ValueError('onboarding audio appeared during conversion; refusing to overwrite it')
        root.mkdir(parents=True)
        for product in PRODUCTS:
            shutil.copytree(stage / product, root / product)
        for key in sorted(KEYS):
            shutil.copyfile(stage / 'an' / (key + '.ogg'), common / (key + '.ogg'))
        (root / 'manifest.json').write_text(
            json.dumps(manifest, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path, help='handoff directory with audio/ and 05-数据/')
    args = parser.parse_args()
    prepare(args.source)
