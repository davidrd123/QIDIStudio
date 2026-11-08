// Public-source LAN transport. Adapted from QIDI Studio c58fdc5 QDSDeviceManager.
// Copyright QIDI Technology; modifications November 2026. AGPL-3.0-only.
#pragma once

#include <websocketpp/config/asio_no_tls_client.hpp>
#include <websocketpp/client.hpp>
#include <nlohmann/json.hpp>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Slic3r { namespace GUI {

// Used only when the private QIDI network module is unavailable.
class QidiLocalDeviceWebSocket {
public:
    std::function<void(const std::string&, const std::string&)> on_status_changed;
    std::function<void(const std::string&, const nlohmann::json&)> on_message_received;
    ~QidiLocalDeviceWebSocket();
    bool connect(const std::string& id, const std::string& url);
    void stopConnection(const std::string& id);
    std::vector<std::string> getConnectionIds();
    void sendSubscribeMessage(const std::string& id);
    void getAllErrorList(const std::string& id);
    void sendCommand(const std::string& id, const std::string& script);
    bool sendCommand(const std::string& id, const std::string& name,
                     const std::string& script, const std::string& method);
    bool sendCommand(const std::string& id, const nlohmann::json& params, const std::string& method);
    void sendActionCommand(const std::string& id, const std::string& action);
private:
    using Client = websocketpp::client<websocketpp::config::asio_client>;
    struct Connection {
        Client client;
        websocketpp::connection_hdl handle;
        std::thread thread;
        std::atomic<bool> stopping{false};
    };
    // Selection and background reconnects may target the same device together.
    std::recursive_mutex m_connection_operations;
    std::mutex m_mutex;
    std::unordered_map<std::string, std::shared_ptr<Connection>> m_connections;
};

}} // namespace Slic3r::GUI
