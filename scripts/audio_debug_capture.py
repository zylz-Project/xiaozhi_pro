"""Receive a timed XDBG audio trace locally; no third-party service is used."""
import argparse
import json
import os
from pathlib import Path
import socket
import struct
import time
import wave

HEADER = struct.Struct('<4sBBBBIIIQHH')


def parse_packet(data):
    if len(data) < HEADER.size:
        raise ValueError('short_header')
    magic, version, stream, channels, flags, sequence, rate, position, clock, frames, size = HEADER.unpack_from(data)
    if magic != b'XDBG' or version != 1 or stream not in (0, 1):
        raise ValueError('unsupported_format')
    if channels not in (1, 2) or flags & ~1 or rate not in (8000, 16000, 24000, 32000, 48000):
        raise ValueError('invalid_format')
    if frames < 1 or size > 640 or size != frames * channels * 2 or len(data) != HEADER.size + size:
        raise ValueError('invalid_payload')
    return {'stream': stream, 'channels': channels, 'vad': bool(flags & 1),
            'sequence': sequence, 'rate': rate, 'position': position,
            'device_us': clock, 'frames': frames}, data[HEADER.size:]


class Capture:
    def __init__(self, output, max_seconds=120):
        if not 1 <= max_seconds <= 120:
            raise ValueError('invalid_duration')
        self.max_seconds = max_seconds
        self.output = Path(output)
        self.output.mkdir(parents=True, exist_ok=False, mode=0o700)
        self.metadata = (self.output/'packets.jsonl').open('w')
        self.files = {}
        self.invalid = 0
        self.reordered = 0
        self.restarted = False
        self.limit_reached = False

    def accept(self, data):
        if self.restarted or self.limit_reached:
            return False
        try:
            info, pcm = parse_packet(data)
        except ValueError:
            self.invalid += 1
            return False
        stream = info['stream']
        if stream not in self.files:
            writer = wave.open(str(self.output/('raw-mr.wav' if stream == 0 else 'processed.wav')), 'wb')
            writer.setnchannels(info['channels'])
            writer.setsampwidth(2)
            writer.setframerate(info['rate'])
            self.files[stream] = {'writer': writer, 'rate': info['rate'], 'channels': info['channels'],
                                  'next': info['position'], 'first': info['position'],
                                  'packets': 0, 'missing_frames': 0,
                                  'last_device_us': info['device_us']}
        state = self.files[stream]
        if info['position'] + info['frames'] - state['first'] > state['rate'] * self.max_seconds:
            self.limit_reached = True
            return False
        if info['device_us'] + 2000000 < state['last_device_us']:
            self.restarted = True
            return False
        gap = info['position'] - state['next']
        if info['rate'] != state['rate'] or info['channels'] != state['channels'] or gap > state['rate'] * 2:
            self.invalid += 1
            return False
        if gap < 0:
            self.reordered += 1
            return False
        if gap:
            state['writer'].writeframesraw(bytes(gap * state['channels'] * 2))
            state['missing_frames'] += gap
        state['writer'].writeframesraw(pcm)
        state['next'] = info['position'] + info['frames']
        state['packets'] += 1
        state['last_device_us'] = max(state['last_device_us'], info['device_us'])
        self.metadata.write(json.dumps(info) + '\n')
        return True

    def summary(self):
        return {'streams': {str(k): {name: value for name, value in state.items() if name != 'writer'}
                            for k, state in self.files.items()},
                'invalid_packets': self.invalid, 'reordered_packets': self.reordered,
                'device_restarted': self.restarted, 'sample_limit_reached': self.limit_reached}

    def close(self):
        for state in self.files.values():
            state['writer'].close()
        self.metadata.close()
        (self.output/'summary.json').write_text(json.dumps(self.summary(), indent=2) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True)
    parser.add_argument('--source', required=True, help='Device IPv4 address')
    parser.add_argument('--port', type=int, default=9000)
    parser.add_argument('--seconds', type=int, default=70, help='Capture time after first valid packet')
    parser.add_argument('--wait', type=int, default=300, help='Maximum wait for first valid packet')
    args = parser.parse_args()
    if not 1 <= args.seconds <= 180 or not 1 <= args.wait <= 600 or not 1 <= args.port <= 65535:
        parser.error('invalid time or port')
    os.umask(0o077)
    capture = Capture(args.output, min(args.seconds, 120))
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024 * 1024)
    udp.bind(('0.0.0.0', args.port))
    udp.settimeout(1)
    deadline = time.monotonic() + args.wait
    received = False
    reported = time.monotonic()
    print('Waiting for device audio', flush=True)
    try:
        while time.monotonic() < deadline:
            try:
                data, peer = udp.recvfrom(2048)
            except socket.timeout:
                continue
            if peer[0] != args.source:
                continue
            if not capture.accept(data):
                if capture.restarted or capture.limit_reached:
                    break
                continue
            if not received:
                received = True
                deadline = time.monotonic() + args.seconds
                print('Receiving device audio', flush=True)
            if time.monotonic() - reported >= 5:
                print(json.dumps(capture.summary()), flush=True)
                reported = time.monotonic()
    finally:
        udp.close()
        capture.close()
    print(json.dumps(capture.summary()), flush=True)
    return 0 if received and not capture.restarted and not capture.limit_reached else 1


if __name__ == '__main__':
    raise SystemExit(main())
