#!/usr/bin/env python3
"""Reproduce the short, descending WZH conversation-exit cue offline."""
import argparse
import array
import math
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_OUTPUT = ROOT / 'main/assets/wzh_sounds/conversation_exit.ogg'


def build(output):
    rate = 24000
    samples = array.array('h', [0]) * (rate * 360 // 1000)
    # Two separated notes with zero-slope envelopes avoid hard onset/cutoff.
    for start_ms, length_ms, frequency in [(0, 140, 784), (170, 190, 440)]:
        start = rate * start_ms // 1000
        count = rate * length_ms // 1000
        for i in range(count):
            t = i / rate
            attack = min(1.0, t / 0.012)
            release = min(1.0, (count - 1 - i) / (rate * 0.065))
            envelope = (0.5 - 0.5 * math.cos(math.pi * attack)) * (
                0.5 - 0.5 * math.cos(math.pi * release))
            tone = math.sin(2 * math.pi * frequency * t)
            tone += 0.12 * math.sin(4 * math.pi * frequency * t)
            samples[start + i] = round(9500 * envelope * tone)
    if sys.byteorder != 'little':
        samples.byteswap()
    output = Path(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([
        'ffmpeg', '-v', 'error', '-nostdin', '-y', '-f', 's16le', '-ar', str(rate),
        '-ac', '1', '-i', 'pipe:0', '-c:a', 'libopus', '-b:a', '32000',
        '-application', 'audio', '-frame_duration', '60', '-map_metadata', '-1',
        str(output),
    ], input=samples.tobytes(), check=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=DEFAULT_OUTPUT)
    build(parser.parse_args().output)
