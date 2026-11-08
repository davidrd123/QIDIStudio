// Public-source LAN transport. Adapted from QIDI Studio c58fdc5 QDSDeviceManager.
// Copyright QIDI Technology; modifications November 2026. AGPL-3.0-only.
#include "QidiLocalDeviceWebSocket.hpp"
#include <cstdlib>

namespace Slic3r { namespace GUI {
using nlohmann::json;

QidiLocalDeviceWebSocket::~QidiLocalDeviceWebSocket()
{
    for (const auto& id : getConnectionIds()) stopConnection(id);
}

bool QidiLocalDeviceWebSocket::connect(const std::string& id, const std::string& url)
{
    std::lock_guard<std::recursive_mutex> operation(m_connection_operations);
    stopConnection(id);
    auto connection = std::make_shared<Connection>();
    auto& client = connection->client;
    client.clear_access_channels(websocketpp::log::alevel::all);
    client.clear_error_channels(websocketpp::log::elevel::all);
    client.init_asio();
    std::weak_ptr<Connection> weak = connection;
    client.set_open_handler([this, id, weak](websocketpp::connection_hdl) {
        auto current = weak.lock();
        if (!current || current->stopping) return;
        if (on_status_changed) on_status_changed(id, "connected");
        sendSubscribeMessage(id);
        getAllErrorList(id);
    });
    client.set_message_handler([this, id, weak](websocketpp::connection_hdl, Client::message_ptr message) {
        auto current = weak.lock();
        if (!current || current->stopping) return;
        auto parsed = json::parse(message->get_payload(), nullptr, false);
        if (!parsed.is_discarded() && on_message_received) on_message_received(id, parsed);
    });
    auto offline = [this, id, weak](websocketpp::connection_hdl) {
        auto current = weak.lock();
        if (current && !current->stopping && on_status_changed) on_status_changed(id, "offline");
    };
    client.set_close_handler(offline);
    client.set_fail_handler(offline);
    websocketpp::lib::error_code error;
    auto pending = client.get_connection(url, error);
    if (error) {
        if (on_status_changed) on_status_changed(id, "offline");
        return false;
    }
    connection->handle = pending->get_handle();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_connections[id] = connection;
        client.connect(pending);
        connection->thread = std::thread([this, id, connection]() {
            try { connection->client.run(); }
            catch (...) {
                if (!connection->stopping && on_status_changed) on_status_changed(id, "offline");
            }
        });
    }
    return true;
}

void QidiLocalDeviceWebSocket::stopConnection(const std::string& id)
{
    std::lock_guard<std::recursive_mutex> operation(m_connection_operations);
    std::shared_ptr<Connection> connection;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto found = m_connections.find(id);
        if (found == m_connections.end()) return;
        connection = found->second;
        connection->stopping = true;
        m_connections.erase(found);
    }
    connection->client.stop();
    if (connection->thread.joinable()) connection->thread.join();
}

std::vector<std::string> QidiLocalDeviceWebSocket::getConnectionIds()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::string> ids;
    for (const auto& item : m_connections) ids.push_back(item.first);
    return ids;
}

bool QidiLocalDeviceWebSocket::sendCommand(const std::string& id, const json& params, const std::string& method)
{
    std::shared_ptr<Connection> connection;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto found = m_connections.find(id);
        if (found == m_connections.end() || found->second->stopping) return false;
        connection = found->second;
    }
    const json request = {{"jsonrpc", "2.0"}, {"id", std::atoi(id.c_str())},
                          {"method", method}, {"params", params}};
    websocketpp::lib::error_code error;
    connection->client.send(connection->handle, request.dump(), websocketpp::frame::opcode::text, error);
    return !error;
}

void QidiLocalDeviceWebSocket::sendSubscribeMessage(const std::string& id)
{
    json objects = json::object();
    for (const char* name : {
        "gcode_move",
        "exclude_object",
        "filament_switch_sensor filament_switch_sensor",
        "fan_generic chamber_circulation_fan",
        "fan_generic cooling_fan",
        "fan_generic auxiliary_cooling_fan",
        "output_pin polar_cooler",
        "print_stats_manager",
        "print_stats",
        "display_status",
        "toolhead",
        "heater_bed",
        "extruder",
        "heater_generic chamber",
        "output_pin caselight",
        "save_variables",
        "aht20_f heater_box1",
        "aht20_f heater_box2",
        "aht20_f heater_box3",
        "aht20_f heater_box4",
        "box_stepper slot0",
        "box_stepper slot1",
        "box_stepper slot2",
        "box_stepper slot3",
        "box_stepper slot4",
        "box_stepper slot5",
        "box_stepper slot6",
        "box_stepper slot7",
        "box_stepper slot8",
        "box_stepper slot9",
        "box_stepper slot10",
        "box_stepper slot11",
        "box_stepper slot12",
        "box_stepper slot13",
        "box_stepper slot14",
        "box_stepper slot15",
        "box_stepper slot16",
        "data",
    }) objects[name] = nullptr;
    sendCommand(id, json{{"objects", std::move(objects)}}, "printer.objects.subscribe");
}

void QidiLocalDeviceWebSocket::getAllErrorList(const std::string& id)
{
    sendCommand(id, "method", "get_all_error_list", "server.extensions.request");
}

void QidiLocalDeviceWebSocket::sendCommand(const std::string& id, const std::string& script)
{
    sendCommand(id, json{{"script", script}}, "printer.gcode.script");
}

bool QidiLocalDeviceWebSocket::sendCommand(const std::string& id, const std::string& name,
                                         const std::string& script, const std::string& method)
{
    return sendCommand(id, json{{name, script}}, method);
}

void QidiLocalDeviceWebSocket::sendActionCommand(const std::string& id, const std::string& action)
{
    sendCommand(id, json::object(), "printer.print." + action);
}

}} // namespace Slic3r::GUI
