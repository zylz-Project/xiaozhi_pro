#!/usr/bin/env python3
"""Replay production motion methods with the repository host hardware substitutes.

Compare a source snapshot against the worktree. Reports concern commanded angles,
not measured servo position, torque, clearance or physical smoothness.
"""
import argparse
import csv
import hashlib
import json
import math
from pathlib import Path
import re
import statistics
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts/tests'))
import test_wzh_motion_output as motion

EMIT = r'''
#include <iostream>
#include <string>
template<size_t N>void Emit(const char*name,int variant,uint32_t ms,const std::array<float,N>&pose){
 std::cout<<name<<'\t'<<variant<<'\t'<<ms;
 for(float p:pose)std::cout<<'\t'<<p;
 std::cout<<'\n';
}
'''


def harness(tree, board):
    motion.ROOT = tree
    motion.COMMON = tree / 'main/boards/common/wzh'
    tester = motion.WzhMotionOutputTest()
    if board != 'wzh_dinosaur':
        program = tester.panda_program(board).split('int main(){')[0]
        program += EMIT + r'''
int main(){
 for(int id=0;id<C_COUNT+BODY_COUNT+int(std::size(kInteractionClips));++id)
  for(int mirror=0;mirror<2;++mirror){
   clock_ms=10000;PandaController c;c.mirror_balance_=mirror?-2:2;
   c.StartClip(id,clock_ms);unsigned steps=0;
   do{
    c.UpdatePlayer(clock_ms);c.OutputPose(c.pose_);
    Emit(("clip:"+std::to_string(id)+":"+c.clip_->name).c_str(),mirror,clock_ms-10000,Values(c.last_output_pose_));
    clock_ms+=20;assert(++steps<3000);
   }while(c.clip_active_);
  }
'''
        mechanism = 'kTail' if board == 'wzh_tailRedPanda' else 'kCrawl'
        rest = '{90,90,150,0,0}' if mechanism == 'kTail' else '{90,90,90,0,0}'
        writer = 'c.OutputPose({p[0],p[1],p[2]});Emit(sound_name.c_str(),100+v,t,Values(c.last_output_pose_));'
    else:
        captured = []
        tester.run_cpp = captured.append
        tester.test_dinosaur_resolved_targets_and_output_for_all_clip_families()
        program = captured[0].split('int main(){')[0]
        program = program.replace('std::vector<std::array<float,5>> samples;',
                                  'std::vector<std::array<float,5>> samples;std::vector<uint32_t> times;')
        program = program.replace('samples.push_back({angles[0]',
                                  'times.push_back(clock_ms);samples.push_back({angles[0]')
        source = (tree / 'main/boards/wzh_dinosaur/dinosaur_controller.cc').read_text()
        tables = source[source.index('using wzh_dino::Keyframe;'):source.index('bool DinosaurController::Initialize(')]
        collections = re.findall(r'constexpr MotionClip (\w+)\[\]', tables)
        program += EMIT + '\nint main(){std::vector<MotionClip>clips;\n'
        for name in collections:
            program += f'clips.insert(clips.end(),std::begin({name}),std::end({name}));\n'
        program += r'''
 for(size_t id=0;id<clips.size();++id)for(int mirror=0;mirror<2;++mirror){
  const auto&clip=clips[id];
  clock_ms=10000;DinosaurController c;
  const bool roar=std::strncmp(clip.name,"roar-",5)==0;c.sky_roar_active_=roar;
  c.PlayClip(clip,c.current_pose_,!roar&&mirror,1.35f,1,0);
  for(size_t i=0;i<c.servo_output_.samples.size();++i)
   Emit(("clip:"+std::to_string(id)+":"+clip.name).c_str(),mirror,c.servo_output_.times[i]-10000,c.servo_output_.samples[i]);
 }
'''
        mechanism, rest = 'kDinosaur', '{70,90,90,90,90}'
        writer = 'c.SetPose({p[0],p[1],p[2],p[3],p[4]});Emit(sound_name.c_str(),100+v,t,Values(c.last_output_pose_));'
    # Reuse actual SoundChoreography and embedded hand-authored pair metadata.
    extra = '#include "wzh_sound_choreography.h"\n#include "' + str(
        tree / 'main/boards' / board / 'wzh_user_pairs_catalog.inc') + '"\n'
    program = program[:program.index('int main(){')] + extra + program[program.index('int main(){'):]
    program += r'''
 using namespace wzh;
 RegisterUserPairs(wzh_user_assets::kPairs,std::size(wzh_user_assets::kPairs));
 const SoundPose rest=__REST__;
 for(unsigned source=0;source<user_pair_count+__ANIMAL_COUNT__;++source){
  const bool paired=source<user_pair_count;
  const auto&score=paired?*user_pairs[source].score:kSoundScores[source-user_pair_count+__SCORE_OFFSET__];
  const std::string sound_name=(paired?"pair:":"flash:")+std::to_string(source)+":"+score.name;
  for(unsigned v=0;v<8;++v){
   SoundPlayback sound;sound.playing=true;sound.audible=true;sound.score=&score;sound.kind=score.kind;
   sound.duration_ms=score.duration_ms;sound.sound_index=paired?kUserPairIndexBase+source:source-user_pair_count+1;
   SoundChoreography player;player.Begin(sound,PetMechanism::__MECHANISM__,rest,v*7919U);
   __CONTROLLER__ c;bool powered=false;
   for(unsigned t=0;t<=player.duration();t+=20){
    clock_ms=10000+t;auto p=player.Sample(t,powered);__WRITER__
   }
  }
 }
}
'''.replace('__REST__', rest).replace('__ANIMAL_COUNT__', '27' if board == 'wzh_dinosaur' else '18').replace('__SCORE_OFFSET__', '19' if board == 'wzh_dinosaur' else '1').replace(
        '__MECHANISM__', mechanism).replace('__CONTROLLER__', 'DinosaurController' if board == 'wzh_dinosaur' else 'PandaController').replace('__WRITER__', writer)
    return program


def summarize(samples):
    axes = len(samples[0][1])
    low = [min(p[a] for _, p in samples) for a in range(axes)]
    high = [max(p[a] for _, p in samples) for a in range(axes)]
    peaks = [0., 0., 0.]
    velocity, acceleration = None, None
    for (t0, p0), (t1, p1) in zip(samples, samples[1:]):
        if t1 <= t0:
            continue  # Identical segment endpoints can be written at the same tick.
        dt = (t1-t0)/1000
        v = [(b-a)/dt for a, b in zip(p0, p1)]
        peaks[0] = max(peaks[0], *map(abs, v))
        if velocity is not None:
            a = [(b-x)/dt for x, b in zip(velocity, v)]
            peaks[1] = max(peaks[1], *map(abs, a))
            if acceleration is not None:
                peaks[2] = max(peaks[2], *(abs(b-x)/dt for x, b in zip(acceleration, a)))
            acceleration = a
        velocity = v
    return {'samples': len(samples), 'duration_ms': samples[-1][0], 'low': low, 'high': high,
            'excursion_sum': sum(b-a for a, b in zip(low, high)),
            'peak_speed_dps': peaks[0], 'peak_acceleration_dps2': peaks[1], 'peak_jerk_dps3': peaks[2]}


def replay(tree, label, output):
    rows, examples = {}, {}
    for board in ['wzh_tailRedPanda', 'wzh_crawlPanda', 'wzh_dinosaur']:
        program = output / f'{label}-{board}.cc'
        program.write_text(harness(tree, board))
        executable = program.with_suffix('')
        subprocess.run(['g++', '-std=c++17', '-O2', '-pthread', '-I', str(tree/'main/boards/common/wzh'),
                        str(program), '-o', str(executable)], check=True, capture_output=True)
        raw = executable.with_suffix('.tsv')
        with raw.open('w') as stream:
            subprocess.run([str(executable)], stdout=stream, check=True)
        grouped = {}
        with raw.open() as stream:
            for parts in csv.reader(stream, delimiter='\t'):
                name, variant, time, *angles = parts
                key = board+'|'+name+'|'+variant
                grouped.setdefault(key, []).append((int(time), list(map(float, angles))))
        for key, samples in grouped.items():
            assert all(math.isfinite(a) for _, p in samples for a in p)
            rows[key] = summarize(samples)
            # Keep representative real controller traces for the interactive review.
            name = key.split('|')[1].rsplit(':', 1)[-1]
            audio_example = key.endswith('|100') and any(
                key.split('|')[1].startswith(prefix)
                for prefix in ['pair:0:', 'pair:2:', 'flash:14:', 'flash:24:'])
            if audio_example or (key.endswith('|0') and (
                    name.startswith('body_') or name.startswith('body-') or
                    name in ['curious_sniff', 'discover-canopy', 'happy-invite'])):
                examples[key] = samples
        print(label, board, len(grouped), 'commanded paths', flush=True)
    return rows, examples


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline-tree', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    def source_fingerprints(tree):
        files=[]
        for directory in ['common/wzh','wzh_tailRedPanda','wzh_crawlPanda','wzh_dinosaur']:
            files.extend(p for p in (tree/'main/boards'/directory).glob('*')
                         if p.suffix in ['.h','.cc','.inc'])
        return {str(p.relative_to(tree)):hashlib.sha256(p.read_bytes()).hexdigest()
                for p in sorted(files)}
    reviewed=source_fingerprints(ROOT)
    before, b_traces = replay(args.baseline_tree.resolve(), 'before', args.output)
    after, a_traces = replay(ROOT, 'after', args.output)
    assert reviewed==source_fingerprints(ROOT), 'Motion sources changed during replay; rerun after edits finish'
    assert before.keys() == after.keys()
    comparisons = []
    for key, current in after.items():
        previous = before[key]
        comparisons.append({'path': key, 'before': previous, 'after': current})
    summary = {'paths': len(comparisons), 'method': 'Production methods / host SDK and servo substitutes; 20 ms commanded samples',
               'physical_validation': False, 'reviewed_source_sha256':reviewed, 'rows': comparisons}
    (args.output/'motion-comparison.json').write_text(json.dumps(summary, ensure_ascii=False, indent=2)+'\n')
    traces = {'before': b_traces, 'after': a_traces}
    (args.output/'motion-traces.json').write_text(json.dumps(traces, ensure_ascii=False)+'\n')
    template = (Path(__file__).with_name('fluid_motion_preview.html')).read_text()
    data = json.dumps(traces, ensure_ascii=False).replace('</', '<\\/')
    (args.output/'motion-preview.html').write_text(template.replace('__MOTION_DATA__', data))
    for board in ['wzh_tailRedPanda','wzh_crawlPanda','wzh_dinosaur']:
        subset = [r for r in comparisons if r['path'].startswith(board+'|')]
        ratios = [r['after']['excursion_sum']/r['before']['excursion_sum'] for r in subset if r['before']['excursion_sum']>1]
        print(board, 'paths=',len(subset), 'median excursion ratio=',round(statistics.median(ratios),4))


if __name__ == '__main__':
    main()
