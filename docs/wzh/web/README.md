# WZH 设备网页

三款使用同一套网页源码。用户端由设备热点提供，开发者管理端监听 `8080`，可从设备热点或局域网访问。日常登录和配网步骤见[家庭快速上手](../product/family_quickstart.md)。

| 入口 | 当前用途 |
| --- | --- |
| `http://192.168.4.1/` | 根据已保存网络、当前网络及会话进入配网、登录或设备详情。已配过网的设备失去上行连接后仍可访问本地功能。 |
| `/setup`、`/done` | 在热点上办理 Wi-Fi 连接、换网与完成后的会话交接。失败时保留原网络凭据。 |
| `/manual` | 在配网前也可以查看使用说明。 |
| `http://192.168.4.1:8080/` | 开发者登录后查看诊断、维护音频和操作相应的测试功能。 |

## 维护入口

- [网页资源](../../../main/assets/wzh_ui/) 包含详情、配网、登录、完成、说明书和开发者页面。`scripts/pack_wzh_ui.py` 为所选产品打包资源，静态资源缓存跟随内容版本。
- [用户路由](../../../main/boards/common/wzh/wzh_user_web.cc) 和 [会话与路由策略](../../../main/boards/common/wzh/wzh_user_web_policy.h) 负责配网、会话、对话、音色、教程查询及恢复出厂。登录状态、上行是否可用及本地页面可达是独立状态。
- [维护态管理](../../../main/boards/common/wzh/wzh_web_console.cc) 负责开发者登录、外部音频和维护。Wi-Fi 实现入口在 [project_components/esp-wifi-connect](../../../project_components/esp-wifi-connect/)。

开发者页的「系统」分别显示片内 RAM 与 PSRAM 的可分配剩余量、开机以来的最低值和最大连续空闲块。对话、4G 重连或复刻播放后刷新页面，查看各自的最低值。诊断导出包含相同字段。

## 需要保持的交互

- 配网提交会产生可查询的任务。手机在热点换信道期间失去响应时，在同一个配网页上恢复查询。设备实际连接成功后才发放完成页凭据。失败或等待期间由页面显示状态，已有网络的试连失败应回到原配置。
- 小恐龙页面分别展示原始 Wi-Fi 连接状态、已选传输和 4G 服务状态。联网操作以板级选中的传输为准。切换发到设备后应继续查询状态。
- 教程状态由固件推进。网页轮询只读进度，真正打开页面才计入教程动作。用户页面对话开始和停止各有入口，重试开始操作应保留正在恢复的会话。
- 音色生成和恢复出厂属于设备后台任务。恢复出厂先清理受管音色，再清教程和 Wi-Fi，成功后重启。云端清理失败时显示错误并保留 Wi-Fi 供用户重试。
- 网页使用本地 HTTP。页面密码、开发者凭据和服务密钥应从设备说明或指定配置取得。维护接口的授权由固件校验。

## 验证

```sh
python3 scripts/run_host_tests.py -p 'test_wzh_ui_login_flow.py'
python3 scripts/run_host_tests.py -p 'test_wzh_user_web_page.py'
python3 scripts/run_host_tests.py -p 'test_wzh_portal*.py'
```

浏览器测试使用 Chrome 和 Playwright。真实手机上的热点保留、换信道、录音和恢复出厂需按[整机验收](../product/product_acceptance.md)检查。[历史页面调整](../archive/2026-11/wzh_web_ui_optimization_plan_2026-11-16.md)记录了早期设计和测量。
