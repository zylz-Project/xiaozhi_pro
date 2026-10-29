#!/usr/bin/env python3
"""Compare real tail-board output ranges and balance across every sound/style."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'scripts/tests'))
from test_wzh_tail_balance import replay_program


def replay(baseline=None):
    with tempfile.TemporaryDirectory() as folder:
        cpp=Path(folder)/'replay.cc';cpp.write_text(replay_program(False));exe=cpp.with_suffix('')
        includes=(['-I',str(baseline/'main/boards/common/wzh')] if baseline else [])+['-I',str(ROOT/'main/boards/common/wzh')]
        subprocess.run(['g++','-std=c++17','-O2',*includes,str(cpp),'-o',str(exe)],check=True)
        lines=subprocess.check_output([str(exe)],text=True).splitlines()
    rows=[]
    keys=['head_right_deg','head_left_deg','tail_left_deg','tail_right_deg','highest_tail_ud_deg','mean_tail_bias_deg','high_tail_ms','left_ms','right_ms']
    for line in lines:
        source,name,variant,lively,*values=line.split('\t')
        rows.append({'source':source,'name':name,'variant':int(variant),'lively':bool(int(lively)),**dict(zip(keys,map(float,values)))})
    return rows


def summarize(rows):
    active=[r for r in rows if r['lively']]
    return {'performances':len(rows),'lively_performances':len(active),
            'mean_head_total_range_deg':sum(r['head_left_deg']+r['head_right_deg'] for r in active)/len(active),
            'mean_tail_total_range_deg':sum(r['tail_left_deg']+r['tail_right_deg'] for r in active)/len(active),
            'mean_highest_tail_ud_deg':sum(r['highest_tail_ud_deg'] for r in active)/len(active),
            'worst_tail_left_right_range_difference_deg':max(abs(r['tail_left_deg']-r['tail_right_deg']) for r in active),
            'worst_absolute_tail_bias_deg':max(abs(r['mean_tail_bias_deg']) for r in active)}


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--baseline',type=Path,required=True);p.add_argument('--output',type=Path,required=True);args=p.parse_args()
    before=replay(args.baseline);after=replay()
    result={'source':'production SoundChoreography + tail-board OutputPose at 20 ms; simulated PWM commands, not hardware',
            'baseline_short_flash_caveat':'Before this change, short unpaired Flash sounds could run the library instead; their before rows characterize the common player only. Paired before rows follow the actual former execution path.',
            'paired_before':summarize([r for r in before if r['source']=='paired']),
            'paired_after':summarize([r for r in after if r['source']=='paired']),
            'before':summarize(before),'after':summarize(after),'files_before':before,'files_after':after}
    args.output.write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n')
    print(json.dumps({k:result[k] for k in ['before','after']},ensure_ascii=False))
