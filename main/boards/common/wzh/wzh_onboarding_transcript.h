#ifndef WZH_ONBOARDING_TRANSCRIPT_H_
#define WZH_ONBOARDING_TRANSCRIPT_H_

#include <cstddef>

namespace wzh {
// Keep these product-specific transcripts synchronized with the audio manifest.
inline const char* OnboardingTranscript(std::size_t step) {
#if CONFIG_BOARD_TYPE_WZH_CRAWLPANDA
#define WZH_TRANSCRIPT(crawl, tail, dinosaur) crawl
#elif CONFIG_BOARD_TYPE_WZH_TAILREDPANDA
#define WZH_TRANSCRIPT(crawl, tail, dinosaur) tail
#elif CONFIG_BOARD_TYPE_WZH_DINOSAUR
#define WZH_TRANSCRIPT(crawl, tail, dinosaur) dinosaur
#else
#define WZH_TRANSCRIPT(crawl, tail, dinosaur) ""
#endif
    static constexpr const char* kTranscripts[] = {
        WZH_TRANSCRIPT(  // intro
            "你好呀，我是小华，你的大熊猫伙伴。我最喜欢你摸我的头，也听得懂小口令。来认识一下吧！",
            "你好呀，我是小武，你的小熊猫伙伴。我最喜欢你摸我的头，也听得懂小口令。来认识一下吧！",
            "你好呀，我是小威，你的恐龙伙伴。我最喜欢你摸我的头，也听得懂小口令。来认识一下吧！"),
        WZH_TRANSCRIPT(  // touch
            "我的头、肚子、背都软乎乎的，你一摸我就开心。来摸摸我的头吧。",
            "嘿嘿，我的小脑袋软乎乎的，来摸摸看呀！我会很开心哦！",
            "别看我个子大，我其实很黏人。你摸摸我的头，我就用头蹭蹭你，尾巴也摇起来。"),
        WZH_TRANSCRIPT(  // motion
            "也可以抱起来轻轻晃一晃，我会挥挥爪子给你看。",
            "抱起来晃一晃，我会摇摇尾巴给你看。",
            "抱起来轻轻晃一晃，我的脑袋就转过来找你。"),
        WZH_TRANSCRIPT(  // cmd1
            "不用连网，我也听得懂话。说一声“摇摇头”，我就摇给你看。",
            "不连网也听得懂。喊一声“摇摇头”，我就摇给你看。",
            "不联网也听得懂。说一声“摇摇头”，我就摇给你看。"),
        WZH_TRANSCRIPT(  // net_invite
            "想跟我聊天、听我讲故事，就用手机帮我连上网。现在不连也行，我还能陪你玩。",
            "想跟我聊天、听我讲故事，或者换个声音，就用手机帮我连上网。现在不连也行，我还能陪你玩。",
            "想跟我聊天、听我讲故事，就用手机帮我连上网。现在不连也行，我还能陪你玩。"),
        WZH_TRANSCRIPT(  // wake
            "连上网以后，想跟我说话，就叫“小华小华”，或者快速按两下电源键。",
            "连上网以后，想跟我说话，就叫“小武小武”，或者快速按两下电源键。",
            "连上网以后，想跟我说话，就叫“小威小威”，或者快速按两下电源键。"),
        WZH_TRANSCRIPT(  // ap
            "如果想连的话，接下来要动手机啦：先打开 Wi-Fi，连上 CrawlPanda 开头的热点。",
            "如果想连的话，接下来要动手机啦：先打开 Wi-Fi，连上 RedPanda 开头的热点。",
            "如果想连的话，接下来要动手机啦：先打开 Wi-Fi，连上 Dino 开头的热点。"),
        WZH_TRANSCRIPT(  // page
            "没弹出页面，就打开浏览器，输入一九二点一六八点四点一，选网络、输密码。",
            "没弹出页面，浏览器输一九二点一六八点四点一，选网络、输密码。",
            "没弹出页面，打开浏览器输入一九二点一六八点四点一，选网络、输密码。"),
        WZH_TRANSCRIPT(  // connected
            "我连上网啦！来，跟我说说话吧。",
            "我连上网啦！来，跟我说说话吧。",
            "我连上网啦！来，跟我说说话吧。"),
        WZH_TRANSCRIPT(  // ability
            "我能陪你聊天、讲故事、放音乐，还能查天气。也能学个新声音，用那个声音陪你说话。",
            "我可以跟你聊天、讲故事、放音乐，也能查天气。还能学个新声音说话。",
            "我能陪你聊天、讲故事、放音乐，还能查天气。也能学个新声音。"),
        WZH_TRANSCRIPT(  // interrupt
            "我说话的时候，先说一声“等一下”，或者“我要说话”，我就停下来听你。",
            "我说话的时候，先说一声“等一下”，或者“我要说话”，我就停下来听你。",
            "我说话的时候，先说一声“等一下”，或者“我要说话”，我就停下来听你。"),
        WZH_TRANSCRIPT(  // clone
            "想让我换个声音，就说“我想复刻音色”。",
            "想让我换个声音，就说“我想复刻音色”。",
            "想让我换个声音，就说“我想复刻音色”。"),
        WZH_TRANSCRIPT(  // farewell
            "玩累了我就会犯困，说声“拜拜”我就睡，也可以按两下电源键。",
            "玩累了我就会犯困，说声“拜拜”我就睡，也可以按两下电源键。",
            "玩累了我就会犯困，说声“拜拜”我就睡，也可以按两下电源键。"),
        WZH_TRANSCRIPT(  // idle_miss
            "你去忙什么啦？我在这儿想你。",
            "好久没听见你啦，我有点想你。",
            "你不在，我就自己玩。有点想你了。"),
    };
#undef WZH_TRANSCRIPT
    return step < sizeof(kTranscripts) / sizeof(kTranscripts[0]) ? kTranscripts[step] : "";
}

}  // namespace wzh
#endif  // WZH_ONBOARDING_TRANSCRIPT_H_
