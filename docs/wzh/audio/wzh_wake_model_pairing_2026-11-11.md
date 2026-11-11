# WZH 唤醒模型与板型匹配

2026-11-11 检查中，工作区生成的固件包含大熊猫板代码，`sdkconfig` 和 `generated_assets.bin` 却选择了小熊猫的 `wn9l_ni3hao3xing1bao3_tts3`（你好星宝）。因此该构建与大熊猫的“你好小安”不匹配。这能解释烧入该构建后说“你好小安”无响应，但不能证明每台实机都刷入了这份固件，也不能单凭此排除其他硬件或运行问题。

推荐使用 `python3 scripts/build.py wzh_crawlPanda --name wzh-crawlpanda` 或 `python3 scripts/build.py wzh_tailRedPanda --name wzh-tailredpanda` 成对选择板型和默认唤醒模型，然后在同一构建目录烧入应用与 `generated_assets.bin`。大熊猫的默认模型为 `wn9_nihaoxiaoan_tts2`，小熊猫为 `wn9l_ni3hao3xing1bao3_tts3`，恐龙为 `wn9_xiaolongxiaolong_tts`。更换板型后直接用 IDE 原有配置构建可能保留上一板型的模型；需要重新按目标板配置，不能只替换应用或只替换模型资源。

`wake_model_check.cmake` 对三款专属模型的交叉误选执行构建时检查；仅有意让某款使用另一款专属唤醒词时，才显式开启 `WZH_ALLOW_CROSS_PRODUCT_WAKE_MODEL`。其他自定义模型及 MultiNet 唤醒不受该交叉检查限制。此检查无法确认设备上实际刷入的应用和资源是否来自同一次构建；仍须在三台设备分别验证待机唤醒、在线对话、音色复刻、人声打断以及重启后唤醒。
