#!/usr/bin/env python3
"""Prepare local WZH prompt packs, never synthesize during firmware builds.

Edge is an optional keyless PREVIEW, not the requested Doubao voices. Import
accepts authorized recordings from any provider. Official web-generation records
can verify the selected source voice; real-device listening remains a separate
acceptance step and is never inferred from filenames.
"""
import argparse
import array
import asyncio
import hashlib
import json
import math
import shutil
import subprocess
import sys
import tempfile

from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from prepare_product_audio import change_header_gain  # noqa: E402  (script-local helper)

ROOT = Path(__file__).resolve().parents[2]
MANIFEST = ROOT / 'main/assets/wzh_system_prompts.json'
# Gated RMS target for spoken prompts, with a hard decoded-peak ceiling. Linear
# gain is applied once at encode; loudnorm's dynamic mode badly mis-leveled the
# short digit clips, so levels are measured and matched explicitly instead.
TARGET_RMS_DBFS = -19.5
PEAK_LIMIT_DBFS = -1.5
MAX_GAIN_DB = 12.0
LEVEL_TOLERANCE_DB = 2.0
# Pitch-preserving slowdown: the product page has no rate control, and the
# stock prompts sounded hurried. Kept mild so syllables stay natural.
SPEECH_TEMPO = 0.92


def load_manifest(path=MANIFEST):
    data = json.loads(Path(path).read_text(encoding='utf-8'))
    if data.get('schema') != 1:
        raise ValueError('Unsupported prompt manifest')
    for key, text in data['prompts'].items():
        if not key or any(c not in 'abcdefghijklmnopqrstuvwxyz_0123456789' for c in key):
            raise ValueError('Unsafe prompt name')
        if not isinstance(text, str) or not text.strip():
            raise ValueError('Empty prompt text')
    return data


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def analyze_level(path):
    """Gated RMS and decoded peak in dBFS; stable for sub-second digit clips."""
    raw = subprocess.run(
        ['ffmpeg', '-v', 'error', '-nostdin', '-i', str(path), '-ac', '1', '-ar', '48000',
         '-f', 's16le', 'pipe:1'], check=True, capture_output=True, timeout=60).stdout
    samples = array.array('h')
    samples.frombytes(raw)
    if not samples:
        raise ValueError(f'{Path(path).name}: no audio samples')
    peak = max(abs(value) for value in samples) / 32768.0
    gate = 10 ** (-50 / 20)
    active = [value / 32768.0 for value in samples if abs(value) / 32768.0 >= gate]
    if not active:
        raise ValueError(f'{Path(path).name}: audio is silent')
    rms = math.sqrt(sum(value * value for value in active) / len(active))
    return 20 * math.log10(rms), 20 * math.log10(max(peak, 1e-9))


def inspect_audio(path, limit):
    info = json.loads(subprocess.run(
        ['ffprobe', '-v', 'error', '-show_streams', '-show_format', '-of', 'json', str(path)],
        check=True, capture_output=True, text=True, timeout=30).stdout)
    streams = info['streams']
    if (len(streams) != 1 or streams[0].get('codec_name') != 'opus'
            or streams[0].get('channels') != 1 or Path(path).read_bytes()[:4] != b'OggS'):
        raise ValueError(f'{path.name}: expected mono Ogg Opus')
    duration = float(info['format']['duration'])
    if not 0.05 <= duration <= limit:
        raise ValueError(f'{path.name}: duration {duration:.2f}s exceeds 0.05..{limit}s')
    # Decode the entire stream too: header probing alone misses truncated data.
    subprocess.run(['ffmpeg', '-v', 'error', '-xerror', '-nostdin', '-i', str(path),
                    '-f', 'null', '-'], check=True, capture_output=True, timeout=30)
    return duration


def verify_web_source(record, voice_label, text):
    if (not isinstance(record, dict)
            or record.get('provider') != 'volcengine-product-tts-web'
            or record.get('source_page') != 'https://www.volcengine.com/product/tts'
            or record.get('voice_label') != voice_label or record.get('text') != text
            or not isinstance(record.get('voice_id'), str) or not record['voice_id'].strip()):
        raise ValueError('Official web source does not match the requested voice/transcript')
    # Only this allowlist is retained; browser sessions, cookies and tokens must
    # never be included in an audio pack, even if present in an input document.
    fields = ('provider', 'source_page', 'voice_label', 'voice_id', 'catalog_preview_id',
              'request_voice_verified', 'text', 'codec', 'sample_rate', 'duration_seconds')
    return {key: record[key] for key in fields if key in record}


def validate_pack(directory, profile, allow_alternative=False, manifest=None):
    manifest = manifest or load_manifest()
    directory = Path(directory)
    pack = json.loads((directory / 'pack.json').read_text(encoding='utf-8'))
    if pack.get('schema') != 1 or pack.get('profile') != profile:
        raise ValueError('Wrong pack schema/product profile')
    if pack.get('requested_voice') != manifest['profiles'][profile]['requested_voice']:
        raise ValueError('Pack requested voice does not match manifest')
    if not isinstance(pack.get('voice_id'), str) or not pack['voice_id'].strip():
        raise ValueError('Missing source voice identity')
    if (not allow_alternative and pack.get('identity_confirmed') is not True
            and pack.get('source_identity_verified') is not True):
        raise ValueError('Requested voice has not been confirmed or its official source verified; '
                         'alternative voices require explicit opt-in')
    if pack.get('provider') == 'edge' and (pack.get('identity_confirmed') is True
                                          or pack.get('source_identity_verified') is True):
        raise ValueError('Edge preview cannot be marked as the requested Doubao voice')
    if set(pack.get('files', {})) != set(manifest['prompts']):
        raise ValueError('Incomplete prompt pack')
    if {p.stem for p in directory.glob('*.ogg')} != set(manifest['prompts']):
        raise ValueError('Missing or unexpected Ogg prompt files')
    for key, text in manifest['prompts'].items():
        path = directory / (key + '.ogg')
        record = pack['files'][key]
        if pack.get('source_identity_verified') is True:
            verify_web_source(record.get('source_metadata'), pack['requested_voice'], text)
        if record.get('text') != text or record.get('sha256') != sha256(path):
            raise ValueError(f'{key}: stale transcript or changed audio checksum')
        limit = manifest['max_duration_seconds'].get(key, manifest['max_duration_seconds']['default'])
        inspect_audio(path, limit)
        # Only packs produced by this pipeline carry level metadata; imported or
        # synthetic fixtures still validate on format, transcript and identity.
        if isinstance(record.get('rms_dbfs'), (int, float)):
            expected_tempo = float(manifest['profiles'][profile].get('tempo', 1.0))
            if abs(float(record.get('tempo', 1.0)) - expected_tempo) > 1e-3:
                raise ValueError(f'{key}: tempo does not match the profile setting')
            rms, peak = analyze_level(path)
            if abs(rms - TARGET_RMS_DBFS) > LEVEL_TOLERANCE_DB:
                raise ValueError(f'{key}: level {rms:.1f} dBFS is outside the unified target')
            if peak > PEAK_LIMIT_DBFS + 1.0:
                raise ValueError(f'{key}: peak {peak:.1f} dBFS leaves no headroom')
    return pack


def convert(source, destination, tempo=SPEECH_TEMPO):
    """Match every prompt to one level and pace without changing pitch.

    Tempo is applied first so level is measured on the audio that is actually
    encoded: time-stretching moves peaks, and measuring the raw source would
    over-drive the loudest clips. Linear gain is then applied once at encode,
    with a small header-gain correction for encoder drift.
    """
    with tempfile.TemporaryDirectory(prefix='wzh-tempo-') as tmp:
        staged = Path(tmp) / 'tempo.wav'
        filters = [f'atempo={tempo:.4f}'] if abs(tempo - 1.0) > 1e-3 else []
        subprocess.run([
            'ffmpeg', '-v', 'error', '-nostdin', '-i', str(source), '-vn', '-ac', '1',
            '-ar', '24000', *(['-af', ','.join(filters)] if filters else []),
            '-c:a', 'pcm_s16le', str(staged)],
            check=True, capture_output=True, timeout=120)
        rms, _ = analyze_level(staged)
        gain = max(-MAX_GAIN_DB, min(MAX_GAIN_DB, TARGET_RMS_DBFS - rms))
        # Loud clips cannot reach the common level without touching the ceiling,
        # so peaks are caught by a gentle limiter instead of holding the whole
        # clip quieter than its neighbours.
        subprocess.run([
            'ffmpeg', '-v', 'error', '-nostdin', '-i', str(staged), '-vn', '-ac', '1',
            '-ar', '24000',
            '-af', f'volume={gain:.2f}dB,alimiter=limit={10 ** (PEAK_LIMIT_DBFS / 20):.4f}:'
                   'attack=5:release=50:level=disabled',
            '-c:a', 'libopus', '-b:a', '32000',
            '-application', 'voip', '-frame_duration', '60', '-f', 'ogg', str(destination)],
            check=True, capture_output=True, timeout=120)
    encoded_rms, encoded_peak = analyze_level(destination)
    correction = TARGET_RMS_DBFS - encoded_rms
    # Never trade headroom for level: clips already near the ceiling stay put.
    correction = min(correction, PEAK_LIMIT_DBFS - encoded_peak)
    if abs(correction) > 0.2:
        correction = max(-3.0, min(3.0, correction))
        destination.write_bytes(change_header_gain(
            destination.read_bytes(), int(round(correction * 256))))
    return analyze_level(destination)[0]


async def edge_source(text, voice, output):
    try:
        import edge_tts
    except ImportError as error:
        raise ValueError('Optional edge-tts package is not installed') from error
    await asyncio.wait_for(edge_tts.Communicate(text, voice).save(str(output)), timeout=60)


def prepare_pack(output, profile, provider, source_dir=None, voice_id=None,
                 confirm_identity=False, manifest=None, verify_official_source=False,
                 keep_masters=False):
    manifest = manifest or load_manifest()
    output = Path(output).resolve()
    if output.exists():
        raise ValueError('Output already exists; choose a new directory (never overwrite a pack)')
    if provider == 'edge' and confirm_identity:
        raise ValueError('Edge voices are alternatives, not 阳光青年/樱桃丸子')
    if provider == 'import' and (not source_dir or not voice_id):
        raise ValueError('Import requires --source-dir and --voice-id')
    if verify_official_source and provider != 'import':
        raise ValueError('Official web-source verification requires imported recordings')
    output.parent.mkdir(parents=True, exist_ok=True)
    # Sources/downloads and converted output are separate fresh directories.
    with tempfile.TemporaryDirectory(prefix='.wzh-prompt-output-', dir=output.parent) as tmp, \
            tempfile.TemporaryDirectory(prefix='wzh-prompt-input-') as inputs:
        stage = Path(tmp)
        profile_info = manifest['profiles'][profile]
        voice = profile_info['edge_preview_voice'] if provider == 'edge' else voice_id
        pack = {'schema': 1, 'profile': profile, 'provider': provider, 'voice_id': voice,
                'requested_voice': profile_info['requested_voice'],
                'identity_confirmed': bool(confirm_identity),
                'source_identity_verified': bool(verify_official_source),
                'listening_confirmed': bool(confirm_identity), 'files': {}}
        for key, text in manifest['prompts'].items():
            if provider == 'edge':
                source = Path(inputs) / (key + '.mp3')
                asyncio.run(edge_source(text, voice, source))
            else:
                matches = [Path(source_dir) / (key + suffix)
                           for suffix in ('.wav', '.mp3', '.ogg', '.flac')
                           if (Path(source_dir) / (key + suffix)).is_file()]
                if len(matches) != 1:
                    raise ValueError(f'{key}: supply exactly one WAV/MP3/Ogg/FLAC master')
                source = matches[0]
            source_metadata = None
            if verify_official_source:
                metadata_path = Path(source_dir) / (key + '.source.json')
                source_metadata = verify_web_source(
                    json.loads(metadata_path.read_text(encoding='utf-8')),
                    profile_info['requested_voice'], text)
            destination = stage / (key + '.ogg')
            tempo = float(profile_info.get('tempo', 1.0))
            level = convert(source, destination, tempo)
            limit = manifest['max_duration_seconds'].get(key, manifest['max_duration_seconds']['default'])
            duration = inspect_audio(destination, limit)
            pack['files'][key] = {'text': text, 'sha256': sha256(destination),
                                  'source_sha256': sha256(source), 'duration_seconds': duration,
                                  'rms_dbfs': round(level, 2), 'tempo': tempo}
            if source_metadata is not None:
                pack['files'][key]['source_metadata'] = source_metadata
            if keep_masters:
                master = stage / 'masters' / (key + source.suffix.lower())
                master.parent.mkdir(exist_ok=True)
                shutil.copyfile(source, master)
                pack['files'][key]['master'] = 'masters/' + master.name
        (stage / 'pack.json').write_text(json.dumps(pack, ensure_ascii=False, indent=2) + '\n',
                                       encoding='utf-8')
        validate_pack(stage, profile, allow_alternative=True, manifest=manifest)
        # Re-check to avoid intentionally replacing a pack created while generating.
        if output.exists():
            raise ValueError('Output appeared while generating; refusing to replace it')
        stage.rename(output)
    return pack


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile', required=True, choices=['dinosaur', 'panda'])
    parser.add_argument('--provider', choices=['edge', 'import'])
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--source-dir', type=Path)
    parser.add_argument('--voice-id')
    parser.add_argument('--confirm-product-voice', action='store_true',
                        help='Import only: recordings were compared with the real product voice')
    parser.add_argument('--verify-official-web-source', action='store_true',
                        help='Import only: verify the recorded official web voice selection/transcript')
    parser.add_argument('--keep-masters', action='store_true',
                        help='Keep original inputs in masters/ (never embedded by CMake)')
    parser.add_argument('--validate', action='store_true')
    parser.add_argument('--allow-alternative', action='store_true')
    args = parser.parse_args()
    try:
        if args.validate:
            validate_pack(args.output, args.profile, args.allow_alternative)
        else:
            if not args.provider:
                parser.error('Generation requires --provider')
            prepare_pack(args.output, args.profile, args.provider, args.source_dir,
                         args.voice_id, args.confirm_product_voice,
                         verify_official_source=args.verify_official_web_source,
                         keep_masters=args.keep_masters)
    except Exception as error:
        # Network exception messages can contain signed URLs; do not log them.
        if isinstance(error, (ValueError, FileNotFoundError)):
            parser.exit(1, f'Prompt pack failed: {error}\n')
        parser.exit(1, f'Prompt pack failed ({type(error).__name__}); '
                       'no valid pack was published. Check service access and audio inputs.\n')
    print(f'Prompt pack validated: {args.output}')


if __name__ == '__main__':
    main()
