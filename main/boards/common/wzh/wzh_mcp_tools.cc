#include "wzh_mcp_tools.h"

#include <cstdio>
#include <string>

#include <esp_log.h>

#include "mcp_server.h"

#include "wzh_audio_hub.h"
#include "wzh_audio_player.h"
#include "wzh_audio_store.h"
#include "wzh_product.h"

#define TAG "WzhMcpTools"

namespace wzh {
namespace {

std::string FormatBytes(uint64_t bytes)
{
    char buf[32];
    if (bytes >= 1024 * 1024) {
        snprintf(buf, sizeof(buf), "%.1fMB", static_cast<double>(bytes) / 1024 / 1024);
    } else {
        snprintf(buf, sizeof(buf), "%lluKB", static_cast<unsigned long long>(bytes / 1024));
    }
    return buf;
}

}  // namespace

void RegisterAudioMcpTools(const char* prefix, AudioStore* store, AudioPlayer* player)
{
    auto& mcp = McpServer::GetInstance();
    const std::string ns = std::string("self.") + prefix;

    mcp.AddTool(ns + ".sound_list",
                "列出设备本地 Flash 中保存的离线声音（动物叫声和环境音）。"
                "用户询问“你都会哪些叫声/有哪些本地声音”时使用。",
                PropertyList(),
                [store](const PropertyList&) -> ReturnValue {
                    const auto files = store->Snapshot();
                    if (files.empty()) {
                        return std::string("本地没有保存任何声音");
                    }
                    std::string result = "共 " + std::to_string(files.size()) + " 个声音：\n";
                    char line[160];
                    for (size_t i = 0; i < files.size(); ++i) {
                        snprintf(line, sizeof(line), "%u: %s [%s, %lu秒]\n",
                                 static_cast<unsigned>(i), files[i].name, files[i].category,
                                 static_cast<unsigned long>(files[i].duration_ms / 1000));
                        result += line;
                    }
                    return result;
                });

    mcp.AddTool(ns + ".sound_play",
                "试听一个本地声音。优先按名称精确匹配；也可以传目录下标。"
                "正在播放其他音频时会拒绝。",
                PropertyList({
                    Property("name", kPropertyTypeString, std::string("")),
                    Property("index", kPropertyTypeInteger, -1, -1, 1000),
                }),
                [store, player](const PropertyList& properties) -> ReturnValue {
                    const auto name = properties["name"].value<std::string>();
                    const auto index = properties["index"].value<int>();
                    int target = -1;
                    if (index >= 0) {
                        target = index;
                    } else if (!name.empty()) {
                        target = store->FindByName(name);
                        if (target < 0) {
                            return std::string("找不到声音：") + name;
                        }
                    } else {
                        return std::string("需要提供 name 或 index");
                    }
                    if (!player->PlayIndex(target)) {
                        return std::string("当前无法播放（正在播放其他音频或写入中）");
                    }
                    return true;
                });

    mcp.AddTool(ns + ".sound_delete",
                "删除一个本地声音文件。用户明确要求删除某个叫声时使用；"
                "删除后数据空间暂不回收，下次音频同步时会自动整理。",
                PropertyList({Property("name", kPropertyTypeString)}),
                [store](const PropertyList& properties) -> ReturnValue {
                    std::string error;
                    const auto name = properties["name"].value<std::string>();
                    if (!store->DeleteFile(name, &error)) {
                        return std::string("删除失败：") + error;
                    }
                    return true;
                });

    mcp.AddTool(ns + ".flash_erase",
                "擦除本地音频 Flash 上的全部声音（不可恢复）。必须由用户明确提出，"
                "并且 confirm 参数必须为 ERASE 才会执行。",
                PropertyList({Property("confirm", kPropertyTypeString)}),
                [store](const PropertyList& properties) -> ReturnValue {
                    if (properties["confirm"].value<std::string>() != "ERASE") {
                        return std::string("需要 confirm=ERASE 才能擦除");
                    }
                    if (store->IsWriting()) {
                        return std::string("另一个写入正在进行，稍后再试");
                    }
                    if (!store->EraseAllAsync([](bool ok) {
                            ESP_LOGI(TAG, "全片擦除%s", ok ? "完成" : "失败");
                        })) {
                        return std::string("无法启动擦除任务");
                    }
                    return std::string("擦除已在后台开始，预计几十秒，可用 flash_status 查询");
                });

    mcp.AddTool(ns + ".flash_status",
                "返回本地音频 Flash 状态：芯片型号、目录格式、文件数、已用/剩余空间。",
                PropertyList(),
                [store](const PropertyList&) -> ReturnValue {
                    char buf[192];
                    snprintf(buf, sizeof(buf),
                             "芯片: %s\n目录: %s\n文件数: %d\n已用: %s\n剩余: %s\n状态: %s",
                             wzh_flash_type_name(),
                             store->layout() == AudioStore::Layout::kV3     ? "v3 (CRC 保护)"
                             : store->layout() == AudioStore::Layout::kLegacy ? "v1/v2 (老工程)"
                                                                              : "空",
                             store->file_count(),
                             FormatBytes(store->used_bytes()).c_str(),
                             FormatBytes(store->free_bytes()).c_str(),
                             store->IsWriting() ? "写入中" : "空闲");
                    return std::string(buf);
                });

    ESP_LOGI(TAG, "Registered audio MCP tools under %s.*", ns.c_str());
}

void RegisterHubMcpTools(const char* prefix, AudioHub* hub)
{
    auto& mcp = McpServer::GetInstance();
    const std::string ns = std::string("self.") + prefix;

    mcp.AddTool(ns + ".sync_status",
                "返回云端音频同步（Audio Hub）状态：是否已绑定、当前状态、"
                "激活码（未绑定时）、本地目录 revision。"
                "用户问“同步状态/怎么绑定设备/激活码”时使用。",
                PropertyList(),
                [hub](const PropertyList&) -> ReturnValue {
                    char buf[256];
                    snprintf(buf, sizeof(buf),
                             "设备ID: %s\n状态: %s\n已绑定: %s\n激活码: %s\n目录版本: %s",
                             hub->device_id().c_str(),
                             AudioHub::StatusName(hub->status()),
                             hub->activated() ? "是" : "否",
                             hub->activated() ? "（已绑定，无需激活码）"
                                              : hub->activation_code().c_str(),
                             hub->revision().empty() ? "（尚未同步）" : hub->revision().c_str());
                    return std::string(buf);
                });

    mcp.AddTool(ns + ".sync_now",
                "立即与云端（Audio Hub）同步一次本地声音。用户明确要求"
                "“同步/更新叫声”时使用。",
                PropertyList(),
                [hub](const PropertyList&) -> ReturnValue {
                    if (hub->status() == AudioHub::Status::kDisabled) {
                        return std::string("Audio Hub 同步未启用");
                    }
                    if (!hub->activated()) {
                        return std::string("设备尚未绑定，激活码：" + hub->activation_code());
                    }
                    hub->RequestSyncNow();
                    return std::string("已请求同步，将在后台执行（对话结束后自动继续）");
                });

    ESP_LOGI(TAG, "Registered hub MCP tools under %s.*", ns.c_str());
}

}  // namespace wzh
