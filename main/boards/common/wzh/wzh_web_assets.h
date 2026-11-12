#ifndef WZH_WEB_ASSETS_H_
#define WZH_WEB_ASSETS_H_

#include <esp_http_server.h>
#include <string_view>

namespace wzh {
// Serve a single file from the embedded, read-only UI bundle.
esp_err_t SendWebAsset(httpd_req_t* req, std::string_view path);
}  // namespace wzh
#endif
