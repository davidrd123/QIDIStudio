// SPDX-License-Identifier: AGPL-3.0-only
// Portable inspection-only MCP integration. See QidiMcpServer.cpp for attribution.
#ifndef slic3r_GUI_QidiMcpServer_hpp_
#define slic3r_GUI_QidiMcpServer_hpp_

#include <memory>

namespace Slic3r::GUI {

// Disabled unless QIDI_STUDIO_MCP=1 and QIDI_STUDIO_MCP_TOKEN is nonempty.
// No model/settings mutations or physical printer operations are exposed.
class QidiMcpServer
{
public:
    QidiMcpServer();
    ~QidiMcpServer();
    QidiMcpServer(const QidiMcpServer&) = delete;
    QidiMcpServer& operator=(const QidiMcpServer&) = delete;

    static bool enabled_by_environment();
    bool start();
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace Slic3r::GUI
#endif
