// Public-source LAN HTTP adapter, November 2026. AGPL-3.0-only.
#pragma once
#include "slic3r/Utils/Http.hpp"

namespace Slic3r { namespace GUI {
class QidiLocalDeviceApi {
public:
    static bool get_file_list(const std::string& base, std::string& body)
    { return get(base + "/api/qidiclient/files/list", body); }
    static bool get_timelapse_directory(const std::string& base, std::string& body)
    { return get(base + "/server/files/directory?root=timelapse&path=timelapse&extended=true", body); }
    static bool get_system_info(const std::string& base, std::string& body)
    { return get(base + "/machine/system_info", body); }
    // The public repository does not publish the private firmware/client-info API.
    static bool get_client_info(const std::string&, std::string& body)
    { body.clear(); return false; }
private:
    static bool get(const std::string& url, std::string& body)
    {
        bool success = false;
        body.clear();
        Http::get(url).timeout_connect(5).timeout_max(15)
            .on_complete([&](std::string response, unsigned status) {
                success = status >= 200 && status < 300;
                if (success) body = std::move(response);
            }).perform_sync();
        return success;
    }
};
using QIDIDeviceApi = QidiLocalDeviceApi;
}} // namespace Slic3r::GUI
