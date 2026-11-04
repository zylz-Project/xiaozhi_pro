#!/usr/bin/env python3
"""Measure source loudness and prepare product Ogg Opus without destructive edits.

Existing Opus packets are never re-encoded: only the standardized header gain
and its Ogg page CRC change. Other input formats use one high-quality encode.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import struct
import subprocess


def measure(data):
    result = subprocess.run(
        ['ffmpeg', '-hide_banner', '-nostdin', '-i', 'pipe:0', '-af',
         'loudnorm=I=-16:TP=-1.5:LRA=11:print_format=json', '-f', 'null', '-'],
        input=data, capture_output=True, check=True, timeout=120)
    match = re.search(rb'\{[^{}]*"input_i"[^{}]*\}', result.stderr, re.S)
    if not match:
        raise ValueError('FFmpeg did not return loudness measurements')
    info = json.loads(match.group())
    return {key: float(info[key]) for key in ['input_i', 'input_tp', 'input_lra']}


def choose_gain(info, target=-16.0, max_boost=6.0, max_cut=4.0, peak=-1.5, protected_playback=False):
    loudness, true_peak = info['input_i'], info['input_tp']
    if not math.isfinite(true_peak):
        return 0
    # Very short/silent sounds have no reliable gated integrated loudness.
    gain = max(-max_cut, min(max_boost, target-loudness)) if math.isfinite(loudness) else 0.0
    if not protected_playback:
        gain = min(gain, peak-true_peak)
    if not math.isfinite(gain):
        return 0
    # Round toward attenuation, retaining the peak margin after quantization.
    return max(-60*256, min(12*256, math.floor(gain*256)))


def ogg_crc(data):
    crc = 0
    for byte in data:
        crc ^= byte << 24
        for _ in range(8):
            crc = ((crc << 1) ^ (0x04c11db7 if crc & 0x80000000 else 0)) & 0xffffffff
    return crc


def change_header_gain(data, additional_q8):
    if data[:4] != b'OggS' or len(data) < 28:
        raise ValueError('Expected Ogg Opus')
    count = data[26]
    start = 27+count
    if len(data) < start or data[start:start+8] != b'OpusHead':
        raise ValueError('OpusHead must begin on the first page')
    end = start+sum(data[27:start])
    if end > len(data) or end < start+19:
        raise ValueError('Truncated Opus header page')
    if data[start+18] != 0 or data[start+9] not in (1, 2):
        raise ValueError('Only mono/stereo Opus mapping family zero is supported')
    original = struct.unpack_from('<h', data, start+16)[0]
    gain = original+additional_q8
    if not -60*256 <= gain <= 12*256:
        raise ValueError('Header gain exceeds the supported product range')
    result = bytearray(data)
    struct.pack_into('<h', result, start+16, gain)
    result[22:26] = bytes(4)
    struct.pack_into('<I', result, 22, ogg_crc(result[:end]))
    return bytes(result)


def fingerprint(data):
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte)*1099511628211) & 0xffffffffffffffff
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--target-lufs', type=float, default=-16.0)
    parser.add_argument('--bitrate', type=int, default=64000)
    parser.add_argument('--force', action='store_true')
    args = parser.parse_args()
    if args.input.resolve() == args.output.resolve():
        parser.error('Output must differ from the original source')
    if args.output.exists() and not args.force:
        parser.error('Output exists; use --force to replace that output')
    if not -24 <= args.target_lufs <= -12 or not 48000 <= args.bitrate <= 128000:
        parser.error('Use a target between -24 and -12 LUFS and 48..128 kbps mono')
    original = args.input.read_bytes()
    if original[:4] == b'OggS':
        opus = original
        transcoded = False
    else:
        opus = subprocess.run(
            ['ffmpeg', '-v', 'error', '-nostdin', '-i', 'pipe:0', '-ac', '1', '-ar', '48000',
             '-c:a', 'libopus', '-application', 'audio', '-b:a', str(args.bitrate),
             '-compression_level', '10', '-vbr', 'on', '-frame_duration', '20', '-f', 'ogg', 'pipe:1'],
            input=original, capture_output=True, check=True, timeout=120).stdout
        transcoded = True
    before = measure(opus)
    gain = choose_gain(before, args.target_lufs)
    output = change_header_gain(opus, gain)
    after = measure(output)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(output)
    # JSON has no nonstandard Infinity/NaN numbers for silence or short clips.
    finite = lambda values: {key: value if math.isfinite(value) else None for key,value in values.items()}
    report = {'input':str(args.input), 'input_sha256':hashlib.sha256(original).hexdigest(),
              'output_sha256':hashlib.sha256(output).hexdigest(), 'encoded_once_from_original':transcoded,
              'opus_payloads_preserved':not transcoded, 'additional_gain_db':gain/256,
              'target_lufs':args.target_lufs, 'before':finite(before), 'after':finite(after)}
    args.output.with_suffix(args.output.suffix+'.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))


if __name__ == '__main__':
    main()
