// SPDX-License-Identifier: AGPL-3.0-only
#include "slic3r/GUI/QidiMcpProtocol.hpp"
#include <cassert>
#include <iostream>
#include <limits>
#include <set>

using namespace Slic3r::GUI::McpProtocol;

static HttpRequest request(const std::string& extra = {}, const std::string& authorization = "Bearer test-token")
{
    return parse_headers("POST /mcp HTTP/1.1\r\nHost: 127.0.0.1:8765\r\n"
                         "Content-Type: application/json\r\nContent-Length: 2\r\n"
                         "Authorization: " + authorization + "\r\n" + extra + "\r\n");
}
static void rejected_http(const std::function<void()>& action, int expected)
{
    bool rejected = false;
    try { action(); } catch (const HttpError& e) { rejected = true; assert(e.status == expected); }
    assert(rejected);
}
static json call(const std::string& name, const json& args = json::object())
{
    return {{"jsonrpc", "2.0"}, {"id", 7}, {"method", "tools/call"},
            {"params", {{"name", name}, {"arguments", args}}}};
}
int main()
{
    validate_http(request(), "test-token");
    validate_http(request("Origin: http://localhost:3210\r\nMCP-Protocol-Version: 2025-06-18\r\n"), "test-token");
    rejected_http([] { validate_http(request("MCP-Protocol-Version: 2025-03-26\r\n"), "test-token"); }, 400);
    rejected_http([] { validate_http(request("Origin: https://attacker.example\r\n"), "test-token"); }, 403);
    rejected_http([] { validate_http(request("Origin: null\r\n"), "test-token"); }, 403);
    rejected_http([] { validate_http(request("MCP-Protocol-Version: 1999-01-01\r\n"), "test-token"); }, 400);
    rejected_http([] { validate_http(request({}, "Bearer wrong-token"), "test-token"); }, 401);
    rejected_http([] { validate_http(request(), ""); }, 401);
    auto bad_host = request(); bad_host.headers["host"] = "attacker.example:8765";
    rejected_http([&] { validate_http(bad_host, "test-token"); }, 403);
    auto bad_type = request(); bad_type.headers["content-type"] = "text/plain";
    rejected_http([&] { validate_http(bad_type, "test-token"); }, 415);
    auto stream = request(); stream.method = "GET";
    rejected_http([&] { validate_http(stream, "test-token"); }, 405);
    auto bad_path = request(); bad_path.target = "/mcp/../file";
    rejected_http([&] { validate_http(bad_path, "test-token"); }, 404);
    auto preflight = request("Origin: http://localhost:3210\r\n", ""); preflight.method = "OPTIONS";
    validate_http(preflight, "test-token");
    preflight.headers.erase("origin");
    rejected_http([&] { validate_http(preflight, "test-token"); }, 403);
    rejected_http([] { parse_headers("POST /mcp HTTP/1.1\r\nHost: localhost:8765\r\nHost: localhost:8765\r\n\r\n"); }, 400);
    rejected_http([] { parse_headers("POST /mcp HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"); }, 400);
    rejected_http([] { parse_headers("POST /mcp HTTP/1.1\r\nContent-Length: -1\r\n\r\n"); }, 400);
    rejected_http([] { parse_headers("POST /mcp HTTP/1.1\r\nContent-Length: 9999999999999999999999999\r\n\r\n"); }, 413);
    for (const auto& origin : {"http://127.0.0.1.evil", "http://localhost@evil", "http://localhost/", "http://localhost:",
                               "http://localhost:65536", "http://localhost:0", "http://localhost:abc", "file://localhost"})
        assert(!local_origin(origin));
    assert(same_token("test-token", "test-token"));
    assert(!same_token("test-token", "test-token-extra"));
    // Header whitespace must be handled in one pass, including the all-space
    // case and a large unauthenticated header within the request size limit.
    assert(trim("").empty());
    assert(trim(" \t ").empty());
    assert(trim(" \tvalue\t ") == "value");
    assert(trim(std::string(max_request_bytes / 2, ' ') + "value") == "value");

    int inspections = 0;
    auto inspect = [&](const std::string& name, const json&) {
        ++inspections;
        assert(find_tool(name));
        return json{{"observed", true}};
    };
    auto rpc = [&](const json& input) { return handle_rpc(input, inspect); };
    const json initialized = rpc({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"},
                                  {"params", {{"protocolVersion", "2025-06-18"}}}});
    assert(initialized["result"]["protocolVersion"] == "2025-06-18");
    assert(initialized["result"]["instructions"].get<std::string>().find("https://github.com/davidrd123/QIDIStudio") != std::string::npos);
    const json negotiated = rpc({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"},
                                 {"params", {{"protocolVersion", "2025-03-26"}}}});
    assert(negotiated["result"]["protocolVersion"] == "2025-06-18");
    assert(rpc({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}).is_null());
    const json listing = rpc({{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}});
    assert(listing["result"]["tools"].size() == registry().size());
    std::set<std::string> names;
    for (const Tool& tool : registry()) {
        assert(names.insert(tool.name).second);
        json args = json::object();
        if (tool.arguments == Tool::Object) args = {{"object_id", 0}};
        if (tool.arguments == Tool::Settings) args = {{"scope", "filament"}, {"filament_index", 0}, {"keys", {"nozzle_temperature"}}};
        const json result = rpc(call(tool.name, args));
        assert(result.contains("result") && result["result"]["isError"] == false);
    }
    const int initial_count = inspections;
    for (const auto& name : {"start_print_job", "slice_plate", "set_slice_settings", "import_model", "capture_printer_camera"}) {
        assert(!find_tool(name));
        assert(rpc(call(name))["error"]["code"] == -32602);
    }
    for (const json& input : {
        call("get_object_state"), call("get_object_state", {{"object_id", "0"}}),
        call("get_object_state", {{"object_id", -1}}), call("get_object_state", {{"object_id", 2147483648LL}}),
        call("get_object_state", {{"object_id", std::numeric_limits<unsigned long long>::max()}}),
        call("get_ui_state", {{"unexpected", true}}), call("get_slice_settings", {{"keys", {"a", "a"}}}),
        call("get_slice_settings", {{"keys", {1}}}), call("get_slice_settings", {{"scope", 0}, {"keys", {"a"}}}),
        call("get_slice_settings", {{"scope", "print"}, {"filament_index", 0}, {"keys", {"a"}}})
    }) assert(rpc(input)["error"]["code"] == -32602);
    json notification_call = call("get_ui_state"); notification_call.erase("id");
    assert(rpc(notification_call).is_null());
    // Even tool-shaped, unknown, and invalid-argument notifications must never
    // execute an inspection or return a JSON-RPC reply.
    notification_call = call("start_print_job"); notification_call.erase("id");
    assert(rpc(notification_call).is_null());
    notification_call = call("get_object_state", {{"object_id", -1}}); notification_call.erase("id");
    assert(rpc(notification_call).is_null());
    assert(rpc({{"jsonrpc", "2.0"}, {"method", "notifications/unknown"}}).is_null());
    json null_id_call = call("get_ui_state"); null_id_call["id"] = nullptr;
    assert(rpc(null_id_call)["error"]["code"] == -32600);
    assert(rpc(json::array({call("get_ui_state"), notification_call}))["error"]["code"] == -32600);
    for (const json& input : {json(), json::array(), json("text"), json(5),
                             json{{"jsonrpc", 2}, {"method", "ping"}},
                             json{{"jsonrpc", "2.0"}, {"id", nullptr}, {"method", "ping"}},
                             json{{"jsonrpc", "2.0"}, {"id", json::array()}, {"method", "ping"}}})
        assert(rpc(input)["error"]["code"] == -32600);
    assert(inspections == initial_count);
    assert(handle_rpc(call("get_ui_state"), [](const std::string&, const json&) -> json {
        throw std::runtime_error("exception barrier");
    })["error"]["code"] == -32603);
    assert(handle_rpc(call("get_ui_state"), [](const std::string&, const json&) -> json {
        return {{"error", "GUI blocked"}};
    })["result"]["isError"] == true);
    assert(handle_rpc(call("get_ui_state"), [](const std::string&, const json&) -> json {
        return {{"error", nullptr}};
    })["result"]["isError"] == false);
    // A failed request must not poison subsequent dispatch.
    assert(rpc(call("get_ui_state"))["result"]["structuredContent"]["observed"] == true);
    std::cout << "MCP protocol checks passed: HTTP boundary, restricted registry, argument validation, and exception recovery\n";
}
