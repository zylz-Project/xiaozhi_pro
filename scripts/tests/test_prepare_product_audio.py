"""Product loudness calibration preserves encoded material and original files."""
from pathlib import Path
import importlib.util
import json
import math
import shutil
import struct
import subprocess
import tempfile
import unittest
import wave

ROOT=Path(__file__).resolve().parents[2]
SPEC=importlib.util.spec_from_file_location('prepare_product_audio', ROOT/'scripts/audio/prepare_product_audio.py')
AUDIO=importlib.util.module_from_spec(SPEC);SPEC.loader.exec_module(AUDIO)


class PrepareProductAudioTest(unittest.TestCase):
    def test_bounded_static_gain_silence_short_clips_and_true_peak_priority(self):
        def gain(loudness,peak):return AUDIO.choose_gain({'input_i':loudness,'input_tp':peak})/256
        self.assertEqual(gain(-30,-20),6)
        self.assertEqual(gain(-8,-1),-4)
        self.assertEqual(gain(-30,-.5),-1)
        self.assertEqual(gain(float('-inf'),float('-inf')),0)
        self.assertEqual(gain(float('-inf'),-5),0)
        self.assertEqual(gain(float('-inf'),-.5),-1)
        self.assertLessEqual(gain(-30,4),-5.5)
        # The firmware applies calibrated gain in float before its peak guard;
        # a single high-crest transient must not prevent a quiet sound's lift.
        self.assertEqual(AUDIO.choose_gain({'input_i':-30,'input_tp':-.5},protected_playback=True),1536)

    def test_gain_metadata_changes_only_header_gain_and_page_crc(self):
        head=bytearray(19);head[:8]=b'OpusHead';head[8]=1;head[9]=1
        struct.pack_into('<H',head,10,312);struct.pack_into('<I',head,12,44100)
        page=bytearray(28);page[:4]=b'OggS';page[5]=2;page[26]=1;page[27]=19
        page.extend(head);struct.pack_into('<I',page,22,AUDIO.ogg_crc(page))
        suffix=b'encoded-opus-payload-and-original-granules'
        original=bytes(page)+suffix;changed=AUDIO.change_header_gain(original,512)
        self.assertEqual(original,bytes(page)+suffix)
        self.assertEqual(changed[len(page):],suffix)
        self.assertEqual(struct.unpack_from('<h',changed,44)[0],512)
        self.assertEqual(changed[38:44],original[38:44]) # preskip and source rate
        checked=bytearray(changed[:len(page)]);expected=struct.unpack_from('<I',checked,22)[0]
        checked[22:26]=bytes(4);self.assertEqual(AUDIO.ogg_crc(checked),expected)
        allowed=set(range(22,26))|{44,45}
        self.assertTrue(all(a==b or i in allowed for i,(a,b) in enumerate(zip(original,changed))))
        for bad in [b'',b'OggS',bytes(page[:30])]:
            with self.assertRaises(ValueError):AUDIO.change_header_gain(bad,0)
        with self.assertRaises(ValueError):AUDIO.change_header_gain(original,13*256)

    def test_export_silent_source_is_finite_does_not_overwrite_or_modify_original(self):
        if not shutil.which('ffmpeg'):self.skipTest('ffmpeg required')
        with tempfile.TemporaryDirectory() as temp:
            folder=Path(temp);source=folder/'source.wav';dest=folder/'prepared.opus'
            with wave.open(str(source),'wb') as w:
                w.setnchannels(1);w.setsampwidth(2);w.setframerate(24000);w.writeframes(bytes(48000))
            original=source.read_bytes()
            command=['python3',str(ROOT/'scripts/audio/prepare_product_audio.py'),str(source),'--output',str(dest)]
            result=subprocess.run(command,capture_output=True,text=True,timeout=30)
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertEqual(source.read_bytes(),original)
            report=json.loads(dest.with_suffix('.opus.json').read_text())
            self.assertEqual(report['additional_gain_db'],0)
            self.assertIsNone(report['before']['input_i'])
            self.assertTrue(dest.read_bytes().startswith(b'OggS'))
            self.assertNotEqual(subprocess.run(command,capture_output=True).returncode,0)
            command[-1]=str(source)
            self.assertNotEqual(subprocess.run(command,capture_output=True).returncode,0)
