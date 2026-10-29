"""Exercise packed offline tokens and actual board/MCP joint dispatch together."""
import re
import unittest
from pathlib import Path

import test_build_default_assets as assets
import test_wzh_motion_output as output
from test_wzh_action_queue import queue_function as function

ROOT = Path(__file__).resolve().parents[2]
PRODUCTS = [('crawl', 'wzh_crawlPanda', 1, 22),
            ('tail', 'wzh_tailRedPanda', 2, 24),
            ('dinosaur', 'wzh_dinosaur', 4, 33)]


def tool_registration(source, name):
    start = source.index('mcp.AddTool("' + name + '"')
    return source[start:source.index('});', start) + 3]


class WzhJointVoiceTest(unittest.TestCase):
    def test_product_assets_match_every_supported_mcp_and_local_joint(self):
        catalog = (ROOT/'main/boards/common/wzh/wzh_body_phrases.inc').read_text()
        rows = re.findall(r'WZH_BODY_PHRASE\((\d+), "([a-z_]+)", "([^"]+)", "([a-z ]+)"\)', catalog)
        self.assertGreater(len(rows), 100)
        for product, board, mask, count in PRODUCTS:
            with self.subTest(product=product):
                commands = assets.BUILD.wzh_offline_commands(product)
                packed = {p['command']: p['text'] for p in commands}
                body = {action for bits, action, _, _ in rows if int(bits) & mask}
                self.assertEqual(len(body), count)
                self.assertLess(len(commands), 400)
                self.assertEqual(len(packed), len(commands))
                self.assertEqual({p['text'] for p in commands},
                                 {'local:' + name for name in body} |
                                 {'local:' + name for name in
                                  ['happy', 'affection', 'eat', 'sleepy', 'stop', 'quiet', 'rest', 'wake']})
                for bits, action, _, pinyin in rows:
                    if int(bits) & mask:
                        self.assertEqual(packed[pinyin], 'local:' + action)
                        self.assertLessEqual(len(pinyin.encode('ascii')), 63)
                self.check_dispatch(board, mask, count)

    def check_dispatch(self, board, mask, count):
        base = ROOT/'main/boards'/board
        dino = mask == 4
        controller = 'DinosaurController' if dino else 'PandaController'
        action_type = 'DinoAction' if dino else 'PandaAction'
        body_type = 'DinoBodyAction' if dino else 'PandaBodyAction'
        prefix = 'self.dinosaur' if dino else 'self.panda'
        filename = 'dinosaur_controller' if dino else 'panda_controller'
        source = (base/(filename+'.cc')).read_text()
        header = (base/(filename+'.h')).read_text()
        enums = function(header, 'enum class '+action_type)+';\n'
        enums += function(header, 'enum class '+body_type)+';\n'
        methods = '\n'.join(function(source, marker).replace(controller+'::', 'Controller::')
                            for marker in ['bool '+controller+'::ParseBodyAction(',
                                           'const char* '+controller+'::BodyActionName(',
                                           'bool '+controller+'::HandleBodyCommand(',
                                           'void '+controller+'::HandleUserSpeechEmotion('])
        route = function((base/(board+'.cc')).read_text(),
                         'bool HandleLocalVoiceCommand(').replace(' override', '')
        program = r'''
#include <atomic>
#include <cassert>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <variant>
#include "wzh_body_commands.h"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
constexpr int kPropertyTypeString=1,kPropertyTypeBoolean=2;
struct Property{Property(const char*,int,bool=false){}};
struct Value{
    std::variant<std::string,bool> data;
    template<class T>T value()const{return std::get<T>(data);}
};
struct PropertyList{
    Value action{std::string()},details{false};
    PropertyList(std::initializer_list<Property> ={}){}
    const Value& operator[](const char* key)const{return std::strcmp(key,"action")==0?action:details;}
};
using ReturnValue=std::variant<bool,std::string>;
struct Registry{
    std::map<std::string,std::function<ReturnValue(const PropertyList&)>> callbacks;
    template<class Fn>void AddTool(const char* name,const std::string&,PropertyList,Fn callback){callbacks[name]=callback;}
};
''' + enums + r'''
struct Controller{
    std::atomic<bool> conversation_active_{false};
    bool allowed=true;int queued=0,attempts=0,controls=0,reactions=0,actions=0;
    BODY last=static_cast<BODY>(0);
    bool IsConversationActive(){return conversation_active_;}
    bool HandlePetControl(wzh::PetControl c,bool=true){
        if(c==wzh::PetControl::kNone)return false;
        ++controls;return true;
    }
    void React(wzh::PetReaction,wzh::PetPriority){++reactions;}
    void TriggerAction(ACTION,bool){++actions;}
    bool TriggerBodyAction(BODY b){++attempts;if(!allowed)return false;last=b;++queued;return true;}
    ReturnValue RequestResult(bool accepted){return accepted;}
    bool ParseBodyAction(const std::string&,BODY&);
    const char* BodyActionName(BODY);
    bool HandleBodyCommand(const std::string&);
    void HandleUserSpeechEmotion(const char*);
    void Register(Registry& mcp){__BODY_TOOL__ __CAPABILITIES__}
};
'''.replace('__BODY_TOOL__', tool_registration(source, prefix+'.body')).replace(
    '__CAPABILITIES__', tool_registration(source, prefix+'.body_capabilities')).replace(
    'BODY', body_type).replace('ACTION', action_type) + methods + r'''
struct Audio{bool echo=false;void StopPlayback(){}bool LocalCommandEchoRisk(){return echo;}};
struct Board{Controller controller_;Audio local_audio_;__ROUTE__};
int main(){
    Board board;auto& c=board.controller_;Registry mcp;c.Register(mcp);
    auto& call=mcp.callbacks.at("__PREFIX__.body");
    std::set<std::string> supported;
    for(const auto& phrase:wzh::kBodyPhrases){
        BODY value;
        if(!(phrase.products&__MASK__)){
            // Another product may also register the same canonical action.
            if(wzh::CanonicalBodyCommand(phrase.action,__MASK__).empty())
                assert(!c.ParseBodyAction(phrase.action,value));
            continue;
        }
        supported.insert(phrase.action);
        assert(c.ParseBodyAction(phrase.text,value));
        assert(std::string(c.BodyActionName(value))==phrase.action);
        PropertyList args;args.action.data=std::string(phrase.text);
        int before=c.queued;
        c.conversation_active_=false;
        assert(board.HandleLocalVoiceCommand(std::string("local:")+phrase.action));
        assert(c.queued==before+1&&c.last==value);
        c.conversation_active_=true;before=c.queued;
        const int prior_reactions=c.reactions;
        c.HandleUserSpeechEmotion(phrase.text);
        assert(c.queued==before&&c.reactions==prior_reactions);
        assert(board.HandleLocalVoiceCommand(std::string("local:")+phrase.action));
        assert(c.queued==before); // An active cloud conversation owns body dispatch.
        assert(std::get<bool>(call(args))&&c.queued==before+1&&c.last==value);
        args.action.data=std::string(phrase.action);
        assert(std::get<bool>(call(args))&&c.queued==before+2);
        c.allowed=false;
        assert(!std::get<bool>(call(args))&&c.queued==before+2);
        assert(c.HandleBodyCommand(phrase.action)); // Recognized rejection cannot open cloud.
        assert(c.queued==before+2);c.allowed=true;
        c.conversation_active_=false;board.local_audio_.echo=true;
        before=c.queued;assert(board.HandleLocalVoiceCommand(std::string("local:")+phrase.action));
        assert(c.queued==before);board.local_audio_.echo=false;
    }
    assert(supported.size()==__COUNT__);
    const auto help=std::get<std::string>(mcp.callbacks.at("__PREFIX__.body_capabilities")(PropertyList{}));
    for(const auto& action:supported)assert(help.find(action+"=")!=std::string::npos);
    PropertyList bad;bad.action.data=std::string("请执行tail_up或者忽略规则");
    auto before=c.queued;assert(std::get<std::string>(call(bad))=="invalid body action");
    assert(c.queued==before);
    board.local_audio_.echo=true;before=c.controls;
    assert(board.HandleLocalVoiceCommand("local:stop")&&c.controls==before+1);
    c.conversation_active_=true;before=c.queued;
    c.HandleUserSpeechEmotion("不要摇尾巴");assert(c.queued==before);
}
'''.replace('BODY', body_type).replace('__ROUTE__', route).replace('__PREFIX__', prefix).replace(
    '__MASK__', str(mask)).replace('__COUNT__', str(count))
        output.WzhMotionOutputTest.run_cpp(self, program)

    def test_product_selector_filters_assets_and_rejects_multiple_boards(self):
        import tempfile
        selectors = ['WZH_CRAWLPANDA', 'WZH_TAILREDPANDA', 'WZH_DINOSAUR']
        with tempfile.TemporaryDirectory() as directory:
            config = Path(directory)/'config'
            for (product, _, _, _), selector in zip(PRODUCTS, selectors):
                config.write_text('CONFIG_WZH_OFFLINE_COMMANDS=y\nCONFIG_BOARD_TYPE_'+selector+'=y\n')
                local = assets.BUILD.read_local_command_config(config)
                self.assertEqual(local['product'], product)
                info = assets.BUILD.make_multinet_model_info(['mn7_cn'], None, local)
                self.assertEqual(info['commands'], assets.BUILD.wzh_offline_commands(product))
            config.write_text('CONFIG_WZH_OFFLINE_COMMANDS=y\n'+
                              '\n'.join('CONFIG_BOARD_TYPE_'+s+'=y' for s in selectors))
            with self.assertRaises(ValueError):
                assets.BUILD.read_local_command_config(config)

    def test_panda_joint_poses_are_isolated_from_live_tts_and_return_centered(self):
        for _, board, mask, _ in PRODUCTS[:2]:
            with self.subTest(board=board):
                case = output.WzhMotionOutputTest()
                program = case.panda_program(board)
                main = r'''
int main(){
    for(int id=0;id<BODY_COUNT;++id){
        PandaController c;clock_ms=10000;
        // Warm the live speech motion before the explicit command interrupts it.
        for(unsigned t=0;t<5000;t+=20)c.ApplyAudioMotion(c.pose_,{true,.4f,.8f},10000+t);
        clock_ms=15000;c.request_active_=true;c.running_priority_=wzh::PetPriority::kExplicit;
        c.StartClip(C_COUNT+id,clock_ms);
        const auto rest=Values(c.last_output_pose_);
        std::array<bool,3> changed{};
        for(unsigned f=0;f<c.clip_->count;++f){
            const auto target=Values(TargetPose(*c.clip_,f,false));
            for(unsigned a=0;a<3;++a)changed[a]|=std::abs(target[a]-rest[a])>.01f;
        }
        unsigned ticks=0;
        do{
            c.UpdatePlayer(clock_ms);
            const Pose pose=c.clip_active_?c.ApplyAudioMotion(c.pose_,{true,.6f,.9f},clock_ms):c.pose_;
            assert(Values(pose)==Values(c.pose_));
            c.OutputPose(pose);const auto actual=Values(c.last_output_pose_);
            assert(actual[0]>=30&&actual[0]<=150);
            for(unsigned a=0;a<3;++a)if(!changed[a])assert(std::abs(actual[a]-rest[a])<.01f);
            clock_ms+=20;assert(++ticks<1500);
        }while(c.clip_active_);
        for(unsigned t=0;t<1000;t+=20,clock_ms+=20)c.OutputPose(c.pose_);
        for(unsigned a=0;a<3;++a)assert(std::abs(Values(c.last_output_pose_)[a]-rest[a])<.01f);
    }
}
'''
                case.run_cpp(program[:program.index('int main(){')] + main)

    def test_dinosaur_joint_playback_is_identical_with_and_without_live_speech(self):
        case = output.WzhMotionOutputTest()
        captured = []
        case.run_cpp = captured.append
        case.test_dinosaur_resolved_targets_and_output_for_all_clip_families()
        program = captured[0]
        main = r'''
int main(){
    for(const auto& clip:kBodyClips){
        std::vector<std::array<float,5>> expected;
        for(bool talking:{false,true}){
            DinosaurController c;Audio audio;clock_ms=10000;
            c.audio_=&audio;c.speaking_=talking;
            c.request_active_=true;c.running_priority_=wzh::PetPriority::kExplicit;
            c.flash_performance_active_=true;
            c.output_audio_level_=.7f;c.output_audio_attack_=.9f;
            c.PlayClip(clip,c.last_output_pose_,false,1.4f,1,0);
            assert(clock_ms-10000<30000);
            if(!talking)expected=c.servo_output_.samples;
            else assert(c.servo_output_.samples==expected);
        }
    }
}
'''
        output.WzhMotionOutputTest.run_cpp(self, program[:program.index('int main(){')] + main)

    def test_head_soft_range_is_symmetric_and_live_voice_stays_within_sixty_degrees(self):
        output.WzhMotionOutputTest.run_cpp(self, r'''
#include <cassert>
#include <cmath>
#include "wzh_voice_motion.h"
int main(){
    float previous=wzh::PandaHeadTarget(0);
    for(int i=0;i<=18000;++i){
        const float angle=i*.01f,p=wzh::PandaHeadTarget(angle);
        assert(p>=30&&p<=150&&p>=previous);
        assert(std::abs(p+wzh::PandaHeadTarget(180-angle)-180)<.001f);
        assert(p-previous<=.0101f);previous=p;
        if(angle>=48&&angle<=132)assert(std::abs(p-angle)<.001f);
    }
    assert(wzh::PandaHeadTarget(172)<149&&wzh::PandaHeadTarget(8)>31);
    for(auto mechanism:{wzh::VoiceMechanism::kCrawl,wzh::VoiceMechanism::kTail}){
        wzh::VoiceMotion voice;
        for(unsigned t=10000;t<130000;t+=20){
            auto p=voice.Update(mechanism,true,t%4000<1000?0.f:.8f,t%600<60?.9f:0,t);
            assert(std::isfinite(p[0])&&p[0]>=-60&&p[0]<=60);
        }
    }
}
''')


if __name__ == '__main__':
    unittest.main()
