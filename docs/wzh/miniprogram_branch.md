# 小程序版产品分支

本分支保存 `wzh-miniprogram/` 及三个板型的配套固件：爬行熊猫
`wzh-crawlpanda`、摇尾小熊猫 `wzh-tailredpanda`、恐龙 `wzh-dinosaur`。小程序
通过共享的 BLE 和语音服务对接设备；三款产品的引脚、身份、动作和供电逻辑
仍由各自板型维护。构建使用 ESP-IDF 6.0.2。

设备的运行中内存应同时观察内部空闲、最大连续块、DMA 和 PSRAM，不能只看总
空闲。可选 `CONFIG_WZH_MEMORY_DIAGNOSTICS` 包含这些数值并增加运行开销；电源键
异常日志区分采样间隔与 ADC 耗时。BLE 通知保留警告与错误，减少高频 INFO 输出。

音色任务的 PSRAM 栈、Flash 调度器与双向流式 TTS 均为独立实验开关，默认关闭。
开启 Flash 调度器前须验证 NVS、OTA、外部 Flash、断电恢复及低内存失败路径；
编译通过和模拟测试不等同于三款硬件验收。更详细的方案和实测边界见
`docs/wzh/audio/voice_memory_latency_strategy.md`。

小程序的本机私有配置及临时二维码不进入 Git；开发者工具账号配置、云端密钥、
设备 Wi-Fi 凭据和已创建音色也不写入仓库。刷写前应核实板型、活动 OTA 槽、
分区表和备份，不能对未知设备执行整片擦除。
