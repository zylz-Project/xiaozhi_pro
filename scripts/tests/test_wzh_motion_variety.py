"""Per-performance diversity, shared landmarks and bounded fair selection."""
import re
import unittest

import test_wzh_motion_output as motion


class WzhMotionVarietyTest(unittest.TestCase):
    run_cpp = motion.WzhMotionOutputTest.run_cpp

    def test_spatial_styles_are_distinct_monotone_and_keep_rest_and_endpoints(self):
        self.run_cpp(r'''
#include <cassert>
#include <cmath>
#include <set>
#include "wzh_motion_profile.h"
int main(){
 for(float center:{70.f,90.f,150.f})for(unsigned axis=0;axis<5;++axis){
  std::set<int> styles;
  for(unsigned seed=1;seed<=4;++seed){
   float previous=-1;
   for(unsigned sample=0;sample<=1800;++sample){
    float angle=sample/10.f;
    float p=wzh::MotionPhraseAngle(angle,center,0,180,seed,axis);
    assert(std::isfinite(p)&&p>=0&&p<=180&&p>previous);previous=p;
    assert((p-center)*(angle-center)>=0);
   }
   assert(wzh::MotionPhraseAngle(0,center,0,180,seed,axis)==0);
   assert(wzh::MotionPhraseAngle(180,center,0,180,seed,axis)==180);
   assert(wzh::MotionPhraseAngle(center,center,0,180,seed,axis)==center);
   styles.insert(int(std::lround(wzh::MotionPhraseAngle(center/2,center,0,180,seed,axis)*100)));
  }
  assert(styles.size()==4);
 }
 for(float angle:{0.f,30.f,70.f,90.f,150.f,180.f})
  assert(wzh::MotionPhraseAngle(angle,90,0,180,0,0)==angle);
}
''')

    def test_rhythm_changes_phrasing_without_changing_the_four_beat_budget(self):
        self.run_cpp(r'''
#include <array>
#include <cassert>
#include <set>
#include "wzh_motion_profile.h"
int main(){
 std::set<std::array<unsigned,4>> rhythms;
 for(unsigned seed=1;seed<=4;++seed){
  std::array<unsigned,4> beats{};unsigned total=0;
  for(unsigned beat=0;beat<4;++beat){
   beats[beat]=wzh::MotionPhraseDuration(1000,seed,beat);total+=beats[beat];
   assert(beats[beat]>=920&&beats[beat]<=1140);
   assert(beats[beat]==wzh::MotionPhraseDuration(1000,seed,beat+4));
  }
  assert(total==4060);rhythms.insert(beats);
 }
 assert(rhythms.size()==4&&wzh::MotionPhraseDuration(1000,0,0)==1000);
 std::set<int> midpoints;
 for(unsigned style=0;style<4;++style){
  float last=-1;
  for(unsigned i=0;i<=1000;++i){
   float value=wzh::MotionPhraseBeat(i/1000.f,style);
   assert(value>=last&&value>=0&&value<=4);last=value;
  }
  assert(wzh::MotionPhraseBeat(0,style)==0&&wzh::MotionPhraseBeat(1,style)==4);
  midpoints.insert(int(wzh::MotionPhraseBeat(.5f,style)*1000));
 }
 assert(midpoints.size()==4);
}
''')

    def test_interleaved_sound_families_keep_full_per_file_coverage(self):
        self.run_cpp(r'''
#include <cassert>
#include <cstdint>
#include "wzh_sound_variants.h"
int main(){
 using namespace wzh;
 SoundVariantSelector<32> selector;
 SoundPlayback sounds[4];
 for(unsigned i=0;i<4;++i)sounds[i].sound_index=i;
 uint32_t entropy=731;int previous=-1;
 for(unsigned round=0;round<50;++round){
  uint32_t seen[4]={};
  for(unsigned draw=0;draw<32;++draw)for(unsigned file=0;file<4;++file){
   entropy=entropy*1664525U+1013904223U;
   int v=selector.Draw(sounds[file],entropy,32,4);
   assert(v>=0&&v<32&&!(seen[file]&(1U<<v)));seen[file]|=1U<<v;
   // Before the final four choices there is always another family available.
   if(previous>=0&&draw<28)assert(v/4!=previous/4);
   previous=v;
  }
  for(auto mask:seen)assert(mask==UINT32_MAX);
 }
 assert(selector.Draw(sounds[0],0,0)==-1);
 assert(selector.Draw(sounds[0],0,33)==-1);
}
''')

    def test_production_pandas_vary_poses_and_complete_within_existing_speed_limits(self):
        for board in ['wzh_tailRedPanda', 'wzh_crawlPanda']:
            with self.subTest(board=board):
                program = self.panda_program(board).split('int main(){')[0]
                program = program.replace('unsigned esp_random(){return 0;}',
                                          'unsigned entropy=0;unsigned esp_random(){return entropy;}')
                program += r'''
#include <set>
int main(){
 unsigned diverse=0;
 for(int id=0;id<C_COUNT+BODY_COUNT+int(std::size(kInteractionClips));++id){
  std::set<std::vector<int>> shapes;
  for(unsigned seed=1;seed<=4;++seed){
   clock_ms=10000;entropy=seed;PandaController c;c.StartClip(id,clock_ms);
   std::vector<int> shape;
   for(uint8_t frame=0;frame<c.clip_->count;++frame){
    const auto pose=Values(TargetPose(*c.clip_,frame,false,seed));
    for(auto axis:pose)shape.push_back(int(std::lround(axis*10)));
   }
   shapes.insert(shape);
   const bool body=std::strncmp(c.clip_->name,"body_",5)==0;
   unsigned steps=0;auto previous=Values(c.last_output_pose_);
   do{
    c.UpdatePlayer(clock_ms);c.OutputPose(c.pose_);auto out=Values(c.last_output_pose_);
    for(unsigned a=0;a<out.size();++a){
     assert(std::isfinite(out[a])&&out[a]>=0&&out[a]<=180);
     assert(std::abs(out[a]-previous[a])<=wzh::kPetServoSpeed*.02f+.001f);
    }
    assert(out[0]>=30&&out[0]<=150);
    previous=out;clock_ms+=20;assert(++steps<2900);
   }while(c.clip_active_);
   if(body&&seed==4)assert(shapes.size()==1);
  }
  if(shapes.size()>=3)++diverse;
 }
 assert(diverse>50); // Many original clips, not a single specially chosen example.
}
'''
                self.run_cpp(program)

    panda_program = motion.WzhMotionOutputTest.panda_program

    def test_production_dinosaur_styles_keep_all_five_axes_bounded(self):
        tester = motion.WzhMotionOutputTest()
        captured = []
        tester.run_cpp = captured.append
        tester.test_dinosaur_resolved_targets_and_output_for_all_clip_families()
        program = captured[0].split('int main(){')[0]
        program = program.replace('unsigned esp_random(){return 0;}',
                                  'unsigned entropy=0;unsigned esp_random(){return entropy;}')
        source = (motion.ROOT/'main/boards/wzh_dinosaur/dinosaur_controller.cc').read_text()
        tables = source[source.index('using wzh_dino::Keyframe;'):
                        source.index('bool DinosaurController::Initialize(')]
        program += '\n#include <set>\nint main(){std::vector<MotionClip>clips;\n'
        for name in re.findall(r'constexpr MotionClip (\w+)\[\]', tables):
            program += f'clips.insert(clips.end(),std::begin({name}),std::end({name}));\n'
        program += r'''
 unsigned diverse=0;
 for(const auto& clip:clips){
  std::set<std::vector<int>> shapes;
  for(unsigned seed=1;seed<=4;++seed){
   clock_ms=10000;entropy=seed;DinosaurController c;
   c.sky_roar_active_=std::strncmp(clip.name,"roar-",5)==0;
   c.PlayClip(clip,c.current_pose_,false,1.35f,1,0);
   assert(c.servo_output_.samples.size()<2900);
   std::array<float,5> previous={70,90,90,90,90};std::vector<int> shape;
   for(const auto& pose:c.servo_output_.samples){
    for(unsigned a=0;a<5;++a){
     assert(std::isfinite(pose[a])&&pose[a]>=(a==3?55:0)&&pose[a]<=180);
     assert(std::abs(pose[a]-previous[a])<=wzh::kPetServoSpeed*.02f+.001f);
     shape.push_back(int(std::lround(pose[a]*10)));
    }
    previous=pose;
   }
   shapes.insert(shape);
  }
  if(shapes.size()>=3)++diverse;
 }
 assert(diverse>50);
 // Inherited axes retain the first styled target exactly, including after
 // several consecutive holds. This exercises actual PlayClip resolution.
 const Keyframe held[]={{100,120,60,110,135,600,MotionEase::kSmooth},
  {kKeep,kKeep,kKeep,kKeep,kKeep,600,MotionEase::kSmooth},
  {kKeep,kKeep,kKeep,kKeep,kKeep,600,MotionEase::kSmooth}};
 const MotionClip one={"keep-style",held,1},all={"keep-style",held,3};
 for(unsigned seed=1;seed<=4;++seed){
  entropy=seed;clock_ms=10000;DinosaurController first;
  auto a=first.PlayClip(one,first.current_pose_,false,1.35f,1,0);
  clock_ms=10000;DinosaurController second;
  auto b=second.PlayClip(all,second.current_pose_,false,1.35f,1,0);
  assert(a.neck_tilt==b.neck_tilt&&a.neck_lean==b.neck_lean&&a.head_turn==b.head_turn&&
         a.tail_ud==b.tail_ud&&a.tail_lr==b.tail_lr);
 }
}
'''
        self.run_cpp(program)


if __name__ == '__main__':
    unittest.main()
