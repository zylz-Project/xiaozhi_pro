"""Indexed recorder sampling must preserve the complete continuous trajectory."""
from pathlib import Path
import unittest

import test_wzh_fluent_motion as motion_tests
from test_wzh_action_queue import queue_function

ROOT = Path(__file__).resolve().parents[2]


class WzhMotionIndexTest(unittest.TestCase):
    def test_all_three_products_match_full_history_with_nonsequential_seeks(self):
        source=(ROOT/'main/boards/common/wzh/wzh_user_pair_motion.h').read_text()
        indexed=queue_function(source,'Pose Filtered(')
        # Independent full-history traversal; keeps the same integration kernel
        # and current pose semantics, and does not consult the sparse index.
        brute=indexed
        start=brute.index('        const float left_edge')
        end=brute.index('            const auto& frame',start)
        brute=brute[:start]+'''        uint32_t start = 0;
        Pose from = origin_, previous = origin_;
        for (unsigned i = 0; i < pair_->frame_count; ++i) {
'''+brute[end:]
        fast=source.replace('Pose FramePose(unsigned index) const {',
                            'Pose FramePose(unsigned index) const { ++::indexed_calls;')
        slow=source.replace(indexed,brute).replace('WZH_USER_PAIR_MOTION_H_', 'WZH_BRUTE_PAIR_MOTION_H_').replace('class UserPairMotion', 'class BrutePairMotion').replace(
            'Pose FramePose(unsigned index) const {', 'Pose FramePose(unsigned index) const { ++::brute_calls;')
        for board in ['wzh_crawlPanda','wzh_tailRedPanda','wzh_dinosaur']:
            with self.subTest(board=board):
                motion_tests.WzhFluentMotionTest.run_cpp(self,
                    '#include <cstdint>\nuint64_t indexed_calls=0,brute_calls=0;\n'+fast+'\n'+slow+
                    '\n#include "'+str(ROOT/'main/boards'/board/'wzh_user_pairs_catalog.inc')+'"\n'+r'''
#include <cassert>
using namespace wzh;
int main(){
    const bool dino=__DINO__,tail=__TAIL__;
    const SoundPose rest=dino?SoundPose{70,90,90,90,90}:tail?SoundPose{90,90,150,0,0}:SoundPose{90,90,90,0,0};
    for(const auto& pair:wzh_user_assets::kPairs){
        for(unsigned variant:{0U,7U,16U,31U}){
            UserPairMotion indexed;BrutePairMotion brute;
            indexed.Begin(&pair,rest,rest,variant,19);brute.Begin(&pair,rest,rest,variant,19);
            assert(indexed.duration()==brute.duration());
            uint32_t random=123456;
            for(unsigned seek=0;seek<80;++seek){
                random=random*1664525U+1013904223U;
                const auto at=random%(indexed.duration()+1);
                auto a=indexed.Sample(at),b=brute.Sample(at);
                for(unsigned axis=0;axis<pair.axes;++axis)assert(std::abs(a[axis]-b[axis])<.0005f);
            }
        }
    }
    assert(indexed_calls<brute_calls); // Work saved, without a wall-clock benchmark.
}
'''.replace('__DINO__','true' if board=='wzh_dinosaur' else 'false').replace('__TAIL__','true' if board=='wzh_tailRedPanda' else 'false'))
