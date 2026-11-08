// SPDX-License-Identifier: AGPL-3.0-only
// Exercises the public LAN fallback against a loopback-only mock printer.
#include "slic3r/GUI/QidiLocalDeviceWebSocket.hpp"
#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <iostream>

using namespace Slic3r::GUI;
using nlohmann::json;
using namespace std::chrono_literals;

int main()
{
    using Server = websocketpp::server<websocketpp::config::asio>;
    Server mock;
    mock.clear_access_channels(websocketpp::log::alevel::all);
    mock.clear_error_channels(websocketpp::log::elevel::all);
    mock.init_asio();
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<json> requests;
    bool connected = false, offline = false, received = false;
    mock.set_message_handler([&](websocketpp::connection_hdl handle, Server::message_ptr message) {
        auto payload = json::parse(message->get_payload());
        {
            std::lock_guard<std::mutex> lock(mutex);
            requests.push_back(payload);
        }
        websocketpp::lib::error_code error;
        // A malformed frame must not prevent the next valid message reaching the client.
        mock.send(handle, "{broken", websocketpp::frame::opcode::text, error);
        mock.send(handle, R"({"jsonrpc":"2.0","id":42,"result":{"status":{"print_stats":{"state":"printing"}}}})",
                  websocketpp::frame::opcode::text, error);
        if (payload.value("method", "") == "test.close")
            mock.close(handle, websocketpp::close::status::normal, "test", error);
        changed.notify_all();
    });
    mock.listen(boost::asio::ip::tcp::endpoint(boost::asio::ip::address_v4::loopback(), 0));
    boost::system::error_code endpoint_error;
    const auto port = mock.get_local_endpoint(endpoint_error).port();
    assert(!endpoint_error);
    mock.start_accept();
    std::thread server([&] { mock.run(); });
    auto wait = [&](auto predicate) {
        std::unique_lock<std::mutex> lock(mutex);
        assert(changed.wait_for(lock, 10s, predicate));
    };
    {
        QidiLocalDeviceWebSocket client;
        client.on_status_changed = [&](const std::string& id, const std::string& status) {
            assert(id == "42");
            std::lock_guard<std::mutex> lock(mutex);
            connected |= status == "connected";
            offline |= status == "offline";
            changed.notify_all();
        };
        client.on_message_received = [&](const std::string& id, const json& message) {
            assert(id == "42" && message["result"]["status"]["print_stats"]["state"] == "printing");
            std::lock_guard<std::mutex> lock(mutex);
            received = true;
            changed.notify_all();
        };
        assert(!client.sendCommand("missing", json::object(), "test.no_connection"));
        const auto url = "ws://127.0.0.1:" + std::to_string(port) + "/websocket";
        assert(client.connect("42", url));
        wait([&] { return connected && received && requests.size() >= 2; });
        {
            std::lock_guard<std::mutex> lock(mutex);
            assert(requests[0]["method"] == "printer.objects.subscribe");
            assert(requests[0]["params"]["objects"].contains("print_stats"));
            assert(requests[0]["params"]["objects"].contains("extruder"));
            assert(requests[1]["method"] == "server.extensions.request");
            assert(requests[1]["params"]["method"] == "get_all_error_list");
        }
        assert(client.getConnectionIds() == std::vector<std::string>{"42"});
        assert(client.sendCommand("42", "script", "MOCK_ONLY", "test.command"));
        assert(client.sendCommand("42", json::object(), "test.close"));
        wait([&] { return offline && requests.size() >= 4; });
        client.stopConnection("42");
        assert(client.getConnectionIds().empty());
        {
            std::lock_guard<std::mutex> lock(mutex);
            connected = received = false;
        }
        assert(client.connect("42", url));
        wait([&] { return connected && received && requests.size() >= 6; });
        {
            std::lock_guard<std::mutex> lock(mutex);
            connected = received = false;
        }
        std::thread selection([&] { assert(client.connect("42", url)); });
        std::thread reconnect([&] { assert(client.connect("42", url)); });
        selection.join();
        reconnect.join();
        wait([&] { return connected && received && requests.size() >= 8; });
        assert(client.getConnectionIds() == std::vector<std::string>{"42"});
        // Destructor must stop and join the active connection thread.
    }
    mock.stop_listening();
    mock.stop();
    server.join();
    std::cout << "Local network checks passed: subscription, command JSON, malformed-message recovery, close, concurrent reconnect, and joined teardown.\n";
}
