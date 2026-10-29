"""Compile the actual panda clip tables and resolver to verify directional commands."""
import shutil
import subprocess
import tempfile
import unittest
import hashlib
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def function_body(source, marker):
    start = source.index(marker)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class WzhMotionClipsTest(unittest.TestCase):
    def test_imported_frames_match_the_reference_snapshot_after_axis_adaptation(self):
        manifest = json.loads((ROOT / "docs/wzh_reference_motion_manifest.json").read_text())
        for board, metadata in manifest["products"].items():
            with self.subTest(board=board):
                source = (ROOT / "main/boards" / board / metadata["header"]).read_text()
                arrays = {}
                for name, body in re.findall(
                        r'constexpr Keyframe (\w+)\[\] = \{(.*?)\};', source, re.S):
                    frames = []
                    for row in re.findall(r'\{([^{}]+)\}', body):
                        values = []
                        for value in row.split(','):
                            value = value.strip().replace('MotionEase::', '')
                            values.append(value if value == 'kKeep' or value.startswith('k')
                                          else int(value))
                        frames.append(values)
                    arrays[name] = frames
                self.assertEqual(len(arrays), metadata["clips"])
                self.assertEqual(sum(map(len, arrays.values())), metadata["frames"])
                digest = hashlib.sha256(json.dumps(
                    arrays, separators=(',', ':'), sort_keys=True).encode()).hexdigest()
                self.assertEqual(digest, metadata["frames_sha256"])

    def check_product(self, board, first_clip):
        compiler = shutil.which("g++")
        if not compiler:
            self.skipTest("g++ is required")
        base = ROOT / "main/boards" / board
        source = (base / "panda_controller.cc").read_text()
        header = (base / "panda_controller.h").read_text()
        types = function_body(header, "namespace wzh_panda")
        tables = source[source.index(first_clip):source.index("bool Contains(")]
        for name in ("panda_reference_motion.h", "panda_story_motion.h"):
            tables = tables.replace(f'#include "{name}"', f'#include "{base / name}"')
        tables = tables.split("constexpr uint32_t kCrawlAltPeriodMs", 1)[0]
        resolver = function_body(source, "void PandaController::StartClip(")
        resolver = resolver[resolver.index("    const Clip* clip"):resolver.index("    clip_ = clip;")]
        resolver = resolver[:resolver.index("    quiet_center_.Reset();")]
        resolver = resolver.replace("return;", "return nullptr;")
        target = function_body(source, "Pose TargetPose(")
        choose = function_body(source, "int PandaController::ChooseClip(")
        program = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <vector>
#include "wzh_pet_behavior.h"
#define ESP_LOGW(...) ((void)0)
'''
        program += types + "\nusing namespace wzh_panda;\n"
        program += "constexpr int kBehaviourCount = static_cast<int>(Behaviour::kCount);\n"
        program += tables + "\n" + target
        axes = ['head','left_arm','right_arm'] if board=='wzh_crawlPanda' else ['head','tail_lr','tail_ud']
        program += '\nstd::array<float,3> Values(const Pose& p){return {'+','.join('p.'+a for a in axes)+'};}\n'

        program += "\nconst Clip* Resolve(int clip_id) {\n" + resolver + "return clip;\n}\n"
        program += r'''
int irnd(int count) {static uint32_t rng=1234567;rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng%count;}
struct PandaController {
    ShuffleBag bags_[kBehaviourCount];int last_clip_=-1;
    int ChooseClip(Behaviour);
};
'''+choose+"\n"
        program += r'''
int main() {
    PandaController chooser;
    bool reachable[C_COUNT]={};
    for(int family=0;family<kBehaviourCount;++family){
        int expected=0;for(auto id:kFamily[family])if(id>=0)++expected;
        assert(expected>0&&expected<=int(kFamilyWidth));
        bool seen[C_COUNT]={};int last=-1;
        for(int request=0;request<100;++request){
            int clip=chooser.ChooseClip(static_cast<Behaviour>(family));
            assert(clip>=0&&clip<C_COUNT);
            if(expected>1)assert(clip!=last);
            bool belongs=false;for(auto id:kFamily[family])if(id==clip)belongs=true;
            assert(belongs);seen[clip]=reachable[clip]=true;last=clip;
        }
        for(auto id:kFamily[family])if(id>=0)assert(seen[id]);
    }
    for(bool seen:reachable)assert(seen); // Newly added stories are actually selected.
    assert(Resolve(-1) == nullptr);
    assert(Resolve(C_COUNT + BODY_COUNT + std::size(kInteractionClips)) == nullptr);
    for (int i = 0; i < C_COUNT; ++i) assert(Resolve(i) == &kClips[i]);
    for (int i = 0; i < BODY_COUNT; ++i) assert(Resolve(C_COUNT + i) == &kBodyClips[i]);
    assert(std::size(kInteractionClips) == wzh::InteractionMotionSelector::kClipCount);
    for (size_t i=0; i<std::size(kInteractionClips); ++i) {
        assert(Resolve(C_COUNT+BODY_COUNT+i) == &kInteractionClips[i]);
        assert(kInteractionClips[i].count >= 3);
        assert(!kInteractionClips[i].mirrorable);
        assert(kInteractionClips[i].frames[0].duration_ms <= 2000);
        assert(TargetPose(kInteractionClips[i],kInteractionClips[i].count-1,false).head == 90);
    }
    for(unsigned event=0;event<wzh::InteractionMotionSelector::kEventCount;++event){
        std::vector<std::vector<std::array<float,3>>> stories;
        for(unsigned variant=0;variant<wzh::InteractionMotionSelector::kVariantCount;++variant){
            const auto& clip=kInteractionClips[variant*wzh::InteractionMotionSelector::kEventCount+event];
            std::vector<std::array<float,3>> poses;
            for(unsigned frame=0;frame<clip.count;++frame){
                auto pose=Values(TargetPose(clip,frame,false));
                if(poses.empty()||poses.back()!=pose)poses.push_back(pose);
            }
            for(const auto& previous:stories)assert(previous!=poses); // No renamed pose duplicates.
            stories.push_back(poses);
        }
    }
    const Clip* left = Resolve(C_COUNT + BODY_LOOK_LEFT);
    const Clip* right = Resolve(C_COUNT + BODY_LOOK_RIGHT);
    assert(std::strcmp(left->name, "body_look_left") == 0);
    assert(std::strcmp(right->name, "body_look_right") == 0);
    assert(!left->mirrorable && !right->mirrorable);
    assert(TargetPose(*left, 0, false).head > 90);
    assert(TargetPose(*right, 0, false).head < 90);
    assert(TargetPose(*left, 0, true).head == TargetPose(*left, 0, false).head);
    for (const auto& family : kFamily) {
        for (auto id : family) assert(id == -1 || (id >= 0 && id < C_COUNT));
    }
    std::vector<Clip> checked(std::begin(kClips),std::end(kClips));
    checked.insert(checked.end(),std::begin(kBodyClips),std::end(kBodyClips));
    checked.insert(checked.end(),std::begin(kInteractionClips),std::end(kInteractionClips));
    for (const auto& clip : checked) {
        assert(clip.count > 0);
        __CLIP_METRICS__
        for (uint8_t frame = 0; frame < clip.count; ++frame) {
            for (bool mirror : {false, true}) {
                const auto pose = TargetPose(clip, frame, mirror);
                assert(pose.head >= 8 && pose.head <= 172);
'''
        if board == "wzh_crawlPanda":
            program += "assert(pose.left_arm >= 8 && pose.left_arm <= 172);\n"
            program += "assert(pose.right_arm >= 8 && pose.right_arm <= 172);\n"
            metrics, tail_assert = "", ""
        else:
            program += "assert(pose.tail_lr >= 6 && pose.tail_lr <= 174);\n"
            program += "assert(pose.tail_ud >= 0 && pose.tail_ud <= 180);\n"
            program += ("min_lr=std::min(min_lr,pose.tail_lr);max_lr=std::max(max_lr,pose.tail_lr);\n"
                        "min_ud=std::min(min_ud,pose.tail_ud);max_ud=std::max(max_ud,pose.tail_ud);\n")
            metrics = "float min_lr=999,max_lr=-999,min_ud=999,max_ud=-999;"
            # Performances still carry the tail; explicit joint commands may
            # deliberately hold it still or request an already-neutral pose.
            tail_assert = 'if(std::strncmp(clip.name,"body_",5)!=0)assert(max_lr-min_lr>=30||max_ud-min_ud>=30);' 
        program += "}}\n" + tail_assert + "\n}}\n"
        program = program.replace("__CLIP_METRICS__", metrics)
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            cpp = directory / "clips.cc"
            cpp.write_text(program)
            executable = directory / "clips"
            result = subprocess.run(
                [compiler, "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror",
                 "-I", str(ROOT / "main/boards/common/wzh"), str(cpp), "-o", str(executable)],
                capture_output=True, text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_crawl_panda_directional_clips_and_limits(self):
        self.check_product("wzh_crawlPanda", "constexpr Keyframe kIdleListen")

    def test_tail_red_panda_directional_clips_and_limits(self):
        self.check_product("wzh_tailRedPanda", "constexpr Keyframe kIdleGlance")


if __name__ == "__main__":
    unittest.main()
