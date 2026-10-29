#!/usr/bin/env python3
"""Measure the Audio Hub pack; author firmware envelope scores and an audit manifest.

Source audio is read only. Requires ffmpeg/ffprobe and Python's standard library.
"""
import argparse
import array
import hashlib
import json
import math
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def kind(name, category):
    if category == "ambient":
        return "Ambient"
    for tokens, value in [(("脚步",), "Steps"), (("咀嚼", "吃竹"), "Chew"),
                          (("喷嚏",), "Sneeze"), (("睡觉", "犯困"), "Sleep"),
                          (("唤醒",), "Stretch"), (("笑声",), "Laugh"),
                          (("回头",), "Search"), (("撒娇", "哼唧", "Cat"), "Affection"),
                          (("叫声音效合集",), "Roar")]:
        if any(t in name for t in tokens):
            return value
    return "Call"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("audio_root", type=Path)
    parser.add_argument("--output", type=Path, default=ROOT / "main/boards/common/wzh/wzh_sound_scores.inc")
    parser.add_argument("--manifest", type=Path, default=ROOT / "docs/wzh_sound_pack_2026-10-19.json")
    args = parser.parse_args()
    manifest, unique = [], {}
    for path in sorted(args.audio_root.rglob("*.opus")):
        meta = json.loads(subprocess.check_output([
            "ffprobe", "-v", "error", "-show_entries", "format=duration:stream=channels,sample_rate",
            "-of", "json", str(path)]))
        pcm = array.array("h", subprocess.check_output([
            "ffmpeg", "-v", "error", "-i", str(path), "-ar", "8000", "-ac", "1", "-f", "s16le", "-"]))
        rms = [math.sqrt(sum(v*v for v in pcm[i:i+800]) / len(pcm[i:i+800]))
               for i in range(0, len(pcm), 800)]
        reference = max(sorted(rms)[int((len(rms)-1)*.95)], 1)
        levels = [round(min(1, v/reference)**.65 * 255) for v in rms]
        # Preserve actual silence. The motion engine smooths these values over frames.
        duration = round(len(pcm) / 8)
        row = dict(path=str(path.relative_to(args.audio_root)), name=path.stem,
                   category=path.parent.name, duration_ms=duration, size=path.stat().st_size,
                   sha256=hashlib.sha256(path.read_bytes()).hexdigest(), kind=kind(path.name, path.parent.name),
                   channels=meta["streams"][0]["channels"], envelope_step_ms=100)
        manifest.append(row)
        if path.stem in unique:
            if unique[path.stem][0]["sha256"] != row["sha256"]:
                raise ValueError(f"Different audio under the same name: {path.stem}")
        else:
            unique[path.stem] = (row, levels)
    output = ["// Authored from measured PCM by scripts/audio/build_wzh_sound_scores.py; do not hand-edit."]
    for i, (_, levels) in enumerate(unique.values()):
        output.append(f"inline constexpr uint8_t kSoundEnvelope{i}[] = {{")
        output.extend("    " + ", ".join(map(str, levels[j:j+32])) + "," for j in range(0, len(levels), 32))
        output.append("};")
    output.append("inline constexpr SoundScore kSoundScores[] = {")
    for i, (row, _) in enumerate(unique.values()):
        seed = int(row["sha256"][:8], 16)
        output.append(f'    {{{json.dumps(row["name"], ensure_ascii=False)}, SoundKind::k{row["kind"]}, '
                      f'{row["duration_ms"]}, 0x{seed:08x}U, kSoundEnvelope{i}, sizeof(kSoundEnvelope{i})}},')
    output.append("};\n")
    args.output.write_text("\n".join(output))
    args.manifest.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n")
    print(f"{len(manifest)} files, {len(unique)} unique scores; panda duplicates verified identical")


if __name__ == "__main__":
    main()
