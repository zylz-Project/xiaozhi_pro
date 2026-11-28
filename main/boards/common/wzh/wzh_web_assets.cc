#include "wzh_web_assets.h"

#include <cstring>
#include <string>

extern const unsigned char wzh_ui_pack_start[] asm("_binary_wzh_ui_pack_bin_start");

namespace wzh {
namespace {
struct Asset {
    const char* name;
    size_t offset;
    size_t size;
    bool gzip;
};
constexpr Asset kAssets[] = {
#include "wzh_ui_manifest.inc"
};
int Hex(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}
std::string Decode(std::string_view value) {
    std::string result;
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            int a = Hex(value[i + 1]), b = Hex(value[i + 2]);
            if (a < 0 || b < 0 || (a == 0 && b == 0))
                return {};
            result.push_back(static_cast<char>((a << 4) | b));
            i += 2;
        } else
            result.push_back(value[i]);
    }
    return result;
}
}  // namespace
esp_err_t SendWebAsset(httpd_req_t* req, std::string_view path) {
    const auto name = Decode(path);
    for (const auto& item : kAssets) {
        if (name != item.name)
            continue;
        const char* type = "application/octet-stream";
        if (name.size() >= 5 && name.compare(name.size() - 5, 5, ".html") == 0)
            type = "text/html; charset=utf-8";
        else if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".css") == 0)
            type = "text/css; charset=utf-8";
        else if (name.size() >= 3 && name.compare(name.size() - 3, 3, ".js") == 0)
            type = "application/javascript; charset=utf-8";
        else if (name.size() >= 5 && name.compare(name.size() - 5, 5, ".webp") == 0)
            type = "image/webp";
        // Only URLs carrying this exact firmware's content revision may be cached.
        // Unversioned and stale URLs must never retain a mismatched response.
        char query[64] = {}, version[32] = {};
        const bool versioned =
            httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
            httpd_query_key_value(query, "v", version, sizeof(version)) == ESP_OK &&
            std::strcmp(version, WZH_UI_REVISION) == 0;
        const bool static_asset = name.rfind("css/", 0) == 0 || name.rfind("js/", 0) == 0 ||
                                  name.rfind("assets/", 0) == 0;
        httpd_resp_set_hdr(
            req, "Cache-Control",
            static_asset && versioned ? "public, max-age=31536000, immutable" : "no-store");
        httpd_resp_set_type(req, type);
        if (item.gzip)
            httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        return httpd_resp_send(req, reinterpret_cast<const char*>(wzh_ui_pack_start + item.offset),
                               item.size);
    }
    return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
}
}  // namespace wzh
