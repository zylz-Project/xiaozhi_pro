# WZH 固定播报与首次引导音包

三款固件内嵌两套离线语音资源。开机叮咚、对话提示、警示和动物声音也使用本地音频。运行时由同一个 AudioService 播放。

| 音包 | 内容 | 构建选型及文案 |
| --- | --- | --- |
| 固定系统播报 | 配网、设备激活、验证码数字、就绪、升级、低电量和关机，共 17 条 | 大熊猫、小熊猫共用 `panda`，恐龙使用 `dinosaur`。 [文案与制作参数](../../../main/assets/wzh_system_prompts.json) · [录音试听](system_prompts_preview.html) |
| 首次引导及陪伴提示 | 13 段首次教程、无人陪伴和高级提示，共 16 条 | 小安用 `an`，星宝用 `xing`，小龙用 `long`。 [引导录音与哈希](../../../main/assets/wzh_onboarding_voices/manifest.json) · [设备使用的文案](../../../main/boards/common/wzh/wzh_onboarding_transcript.h) |

两套音包都由 `CONFIG_WZH_SYSTEM_PROMPTS` 启用。CMake 按板型选择 Ogg/Opus，同名文件沿用原有 `Lang::Sounds::OGG_*` 符号。原始 MP3 与来源清单用于维护，只有对应 Ogg 编入固件。 [构建选型](../../../main/CMakeLists.txt) · [固定播报校验](../../../scripts/cmake/wzh_system_prompts.cmake)

## 播放时序

- 首次教程先演示触摸、拿起、动作口令和唤醒。Wi-Fi 流程需要时等待手机进入热点页面。恐龙选择 4G 时略过 Wi-Fi 相关提示。网络可用后，首次双击继续联网介绍，再进入对话。设备按有效进度恢复，引导任务或进度记录出错时允许继续常规使用。 [引导实现](../../../main/boards/common/wzh/wzh_onboarding.cc)
- 配网提示等待开机音画及已预留的本地播放。设备激活播报先停止冲突音效，在同一播放代次播放引导和验证码数字。 [系统播报任务](../../../main/boards/common/wzh/wzh_system_prompts.cc) · [应用流程](../../../main/application.cc)
- 稳定低电时在空闲且未录音或维护时播放提醒，持续低电每 10 分钟最多一次。提示被打断且未自然播完，可在下次空闲重试。
- 长按关机先停止互动，保护允许时有限归中并释放舵机。告别语音的入队与排空共享 8 秒上限，电源任务继续监控欠压。临界欠压直接切断电源。 [电源保护](../hardware/sensing_and_protection.md)

## 校验与替换

构建主机需要 Python、ffmpeg 和 ffprobe。固定播报校验格式、时长、文案、来源和哈希。首次引导校验文件集合、Ogg 标识、文案与哈希。

```sh
python3 scripts/audio/prepare_wzh_system_prompts.py --validate \
  --profile dinosaur --output main/assets/wzh_system_voices/dinosaur
python3 scripts/audio/prepare_wzh_system_prompts.py --validate \
  --profile panda --output main/assets/wzh_system_voices/panda

python3 scripts/audio/validate_wzh_onboarding.py an
python3 scripts/audio/validate_wzh_onboarding.py xing
python3 scripts/audio/validate_wzh_onboarding.py long
```

更新固定播报时，按文案清单提供每个键的 MP3 和同名 `*.source.json`，记录来源、音色与实际文案。现有音包使用火山引擎产品体验页生成的恐龙「阳光青年」、熊猫「樱桃丸子」录音。制作脚本在产品目标语速下按 −19.5 dBFS 门限 RMS 调整，保留 −1.5 dBFS 峰值余量。 [固定播报制作脚本](../../../scripts/audio/prepare_wzh_system_prompts.py) · [合成产品说明](https://www.volcengine.com/product/tts)

```sh
python3 scripts/audio/prepare_wzh_system_prompts.py --profile panda --provider import \
  --source-dir /path/to/recordings --voice-id '<实际音色标识>' \
  --verify-official-web-source --keep-masters \
  --output main/assets/wzh_system_voices/panda
```

更新首次引导时，按产品目录和现有键生成 Ogg，更新清单和固件使用的对应文案。先校验三套音包，再分别构建。实机检查语速、数字清晰度、首字和尾字，覆盖首开机、老设备升级、Wi-Fi 和 4G、进入/退出教程、低电与关机。 [首次引导制作脚本](../../../scripts/audio/prepare_wzh_onboarding.py) · [整机验收](../product/product_acceptance.md)
