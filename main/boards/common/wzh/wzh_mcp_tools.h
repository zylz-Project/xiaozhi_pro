#ifndef WZH_MCP_TOOLS_H_
#define WZH_MCP_TOOLS_H_

namespace wzh {

class AudioStore;
class AudioPlayer;
class AudioHub;

// 注册产品级音频管理 MCP 工具（self.<prefix>.sound_list / sound_play /
// sound_delete / flash_erase / flash_status）。Web 控制台取消后，这是
// 用户通过小智对话管理 Flash 音频的入口。
void RegisterAudioMcpTools(const char* prefix, AudioStore* store, AudioPlayer* player);

// 注册 Audio Hub 状态/手动同步工具（self.<prefix>.sync_status / sync_now）。
void RegisterHubMcpTools(const char* prefix, AudioHub* hub);

}  // namespace wzh

#endif  // WZH_MCP_TOOLS_H_
