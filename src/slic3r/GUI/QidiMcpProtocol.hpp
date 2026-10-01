// SPDX-License-Identifier: AGPL-3.0-only
// Protocol boundary for the portable inspection-only QIDI Studio MCP integration.
// Based on the MCP integration by sslinkyy (https://github.com/sslinkyy/QIDIStudio),
// with a restricted registry and independently testable request validation.
// Source commit: d85c905619f6637eed24b6967a4a8659ab3641bf. Modified 2026-09-30.
#ifndef slic3r_GUI_QidiMcpProtocol_hpp_
#define slic3r_GUI_QidiMcpProtocol_hpp_

#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cctype>
#include <functional>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>

namespace Slic3r::GUI::McpProtocol {
using json = nlohmann::json;
constexpr unsigned short port = 8765;
constexpr std::size_t max_request_bytes = 1024 * 1024;

struct HttpError : std::runtime_error {
    int status;
    HttpError(int status_, const char* message) : std::runtime_error(message), status(status_) {}
};
struct HttpRequest {
    std::string method, target, body;
    std::map<std::string, std::string> headers;
    std::size_t content_length{0};
};
inline std::string trim(std::string text)
{
    auto whitespace = [](unsigned char c) { return c == ' ' || c == '\t'; };
    const auto first = std::find_if_not(text.begin(), text.end(), whitespace);
    const auto last = std::find_if_not(text.rbegin(), text.rend(), whitespace).base();
    return first < last ? std::string(first, last) : std::string();
}
inline std::string lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
    return text;
}
inline std::string header(const HttpRequest& request, const char* name)
{
    auto it = request.headers.find(name);
    return it == request.headers.end() ? std::string() : it->second;
}
inline bool supported_version(const std::string& version)
{
    // This implementation accepts individual messages. Older revisions require
    // receiving JSON-RPC batches, which are outside this inspection port.
    return version == "2025-06-18";
}
inline HttpRequest parse_headers(const std::string& bytes)
{
    if (bytes.size() > max_request_bytes) throw HttpError(413, "Request is too large");
    std::istringstream stream(bytes);
    HttpRequest request;
    std::string line, version, extra;
    if (!std::getline(stream, line)) throw HttpError(400, "Missing HTTP request line");
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::istringstream first(line);
    if (!(first >> request.method >> request.target >> version) || first >> extra || version != "HTTP/1.1")
        throw HttpError(400, "Invalid HTTP request line");
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;
        const auto colon = line.find(':');
        if (colon == std::string::npos || colon == 0 || line.front() == ' ' || line.front() == '\t')
            throw HttpError(400, "Invalid HTTP header");
        std::string name = lower(line.substr(0, colon));
        if (!std::all_of(name.begin(), name.end(), [](unsigned char c) {
            return std::isalnum(c) || c == '-';
        })) throw HttpError(400, "Invalid HTTP header name");
        const std::string value = trim(line.substr(colon + 1));
        if (std::any_of(value.begin(), value.end(), [](unsigned char c) { return c < 32 && c != '\t'; }))
            throw HttpError(400, "Invalid HTTP header value");
        if (!request.headers.emplace(name, value).second) throw HttpError(400, "Duplicate HTTP header");
    }
    if (!header(request, "transfer-encoding").empty()) throw HttpError(400, "Transfer-Encoding is unsupported");
    const auto length = request.headers.find("content-length");
    if (length != request.headers.end()) {
        if (length->second.empty() || !std::all_of(length->second.begin(), length->second.end(), [](unsigned char c) {
            return c >= '0' && c <= '9';
        })) throw HttpError(400, "Invalid Content-Length");
        for (unsigned char c : length->second) {
            if (request.content_length > (max_request_bytes - (c - '0')) / 10)
                throw HttpError(413, "Request is too large");
            request.content_length = request.content_length * 10 + (c - '0');
        }
    }
    return request;
}
inline bool local_origin(const std::string& origin)
{
    std::string authority;
    if (origin.rfind("http://", 0) == 0) authority = origin.substr(7);
    else if (origin.rfind("https://", 0) == 0) authority = origin.substr(8);
    else return false;
    if (authority.find_first_of("/@?#") != std::string::npos) return false;
    const auto colon = authority.find(':');
    const std::string host = lower(authority.substr(0, colon));
    if (host != "localhost" && host != "127.0.0.1") return false;
    if (colon == std::string::npos) return true;
    const std::string number = authority.substr(colon + 1);
    if (number.empty() || number.size() > 5) return false;
    unsigned int value = 0;
    for (unsigned char c : number) {
        if (c < '0' || c > '9') return false;
        value = value * 10 + (c - '0');
    }
    return value > 0 && value <= 65535;
}
inline bool same_token(const std::string& supplied, const std::string& expected)
{
    std::size_t different = supplied.size() ^ expected.size();
    for (std::size_t i = 0; i < std::max(supplied.size(), expected.size()); ++i)
        different |= (i < supplied.size() ? static_cast<unsigned char>(supplied[i]) : 0) ^
                     (i < expected.size() ? static_cast<unsigned char>(expected[i]) : 0);
    return different == 0 && !expected.empty();
}
inline void validate_http(const HttpRequest& request, const std::string& token)
{
    const std::string host = lower(header(request, "host"));
    if (host != "127.0.0.1:8765" && host != "localhost:8765")
        throw HttpError(403, "Host must name the local MCP endpoint");
    auto origin = request.headers.find("origin");
    if (origin != request.headers.end() && !local_origin(origin->second))
        throw HttpError(403, "Origin is not allowed");
    const std::string version = header(request, "mcp-protocol-version");
    if (!version.empty() && !supported_version(version)) throw HttpError(400, "Unsupported MCP protocol version");
    // Browser preflight contains the header names, rather than the bearer value.
    if (request.method == "OPTIONS") {
        if (request.target != "/mcp") throw HttpError(404, "Endpoint not found");
        if (origin == request.headers.end()) throw HttpError(403, "Preflight requires a local Origin");
        return;
    }
    const std::string authorization = header(request, "authorization");
    if (authorization.rfind("Bearer ", 0) != 0 || !same_token(authorization.substr(7), token))
        throw HttpError(401, "Bearer authorization is required");
    if (request.target != "/mcp") throw HttpError(404, "Endpoint not found");
    if (request.method != "POST") throw HttpError(405, "POST is required");
    const std::string content_type = lower(header(request, "content-type"));
    if (trim(content_type.substr(0, content_type.find(';'))) != "application/json")
        throw HttpError(415, "Content-Type must be application/json");
    if (request.headers.find("content-length") == request.headers.end())
        throw HttpError(400, "Content-Length is required");
}

struct Tool {
    const char* name;
    const char* description;
    enum Arguments { None, Object, Settings } arguments;
};
inline const std::array<Tool, 11>& registry()
{
    static const std::array<Tool, 11> tools{{
        {"get_ui_state", "Inspect the selected view and visible modal dialogs.", Tool::None},
        {"get_project_state", "Inspect project identity, dirty flags, and plate/object counts.", Tool::None},
        {"list_plates", "Inspect the project build plates.", Tool::None},
        {"get_plate_state", "Inspect the active plate and its instance transforms.", Tool::None},
        {"list_objects", "Inspect model geometry counts and instance transforms.", Tool::None},
        {"get_object_state", "Inspect one model object's geometry and transforms.", Tool::Object},
        {"list_object_volumes", "Inspect one model object's volumes and filament assignments.", Tool::Object},
        {"get_active_presets", "Inspect active preset names and whether each edited preset is dirty.", Tool::None},
        {"get_slice_settings", "Read effective edited print/printer or assigned filament settings by key.", Tool::Settings},
        {"get_slice_status", "Inspect the active plate's current slicing state.", Tool::None},
        {"get_slice_result", "Inspect completed slicing estimates and warnings.", Tool::None}
    }};
    return tools;
}
inline const Tool* find_tool(const std::string& name)
{
    for (const Tool& tool : registry()) if (name == tool.name) return &tool;
    return nullptr;
}
inline json schema(const Tool& tool)
{
    json properties = json::object(), required = json::array();
    if (tool.arguments == Tool::Object) {
        properties["object_id"] = {{"type", "integer"}, {"minimum", 0}};
        required.push_back("object_id");
    } else if (tool.arguments == Tool::Settings) {
        properties = {
            {"scope", {{"type", "string"}, {"enum", {"print", "filament", "printer"}}, {"default", "print"}}},
            {"keys", {{"type", "array"}, {"minItems", 1}, {"maxItems", 256}, {"uniqueItems", true},
                      {"items", {{"type", "string"}, {"minLength", 1}}}}},
            {"filament_index", {{"type", "integer"}, {"minimum", 0}}}
        };
        required.push_back("keys");
    }
    json result = {{"type", "object"}, {"properties", properties}, {"additionalProperties", false}};
    if (!required.empty()) result["required"] = required;
    return result;
}
inline json list_tools()
{
    json tools = json::array();
    for (const Tool& tool : registry()) tools.push_back({
        {"name", tool.name}, {"description", tool.description}, {"inputSchema", schema(tool)},
        {"outputSchema", {{"type", "object"}}},
        {"annotations", {{"readOnlyHint", true}, {"destructiveHint", false},
                          {"idempotentHint", true}, {"openWorldHint", false}}}
    });
    return {{"tools", std::move(tools)}};
}
inline void validate_arguments(const Tool& tool, const json& arguments)
{
    if (!arguments.is_object()) throw std::invalid_argument("Tool arguments must be an object");
    const json definition = schema(tool);
    for (auto it = arguments.begin(); it != arguments.end(); ++it)
        if (!definition["properties"].contains(it.key())) throw std::invalid_argument("Unknown tool argument");
    auto nonnegative_index = [&](const char* key) {
        if (!arguments.contains(key) || !arguments[key].is_number_integer())
            throw std::invalid_argument(std::string(key) + " must be a non-negative integer");
        const auto number = arguments[key].get<long long>();
        if (number < 0 || number > 2147483647LL)
            throw std::invalid_argument(std::string(key) + " is out of range");
    };
    if (tool.arguments == Tool::Object) nonnegative_index("object_id");
    if (tool.arguments == Tool::Settings) {
        const std::string scope = arguments.value("scope", "print");
        if (scope != "print" && scope != "filament" && scope != "printer")
            throw std::invalid_argument("scope must be print, filament, or printer");
        if (!arguments.contains("keys") || !arguments["keys"].is_array() ||
            arguments["keys"].empty() || arguments["keys"].size() > 256)
            throw std::invalid_argument("keys must contain between 1 and 256 settings");
        std::map<std::string, bool> seen;
        for (const json& key : arguments["keys"]) {
            if (!key.is_string() || key.get<std::string>().empty() || !seen.emplace(key.get<std::string>(), true).second)
                throw std::invalid_argument("Setting keys must be nonempty unique strings");
        }
        if (arguments.contains("filament_index")) {
            nonnegative_index("filament_index");
            if (scope != "filament") throw std::invalid_argument("filament_index requires filament scope");
        }
    }
}
inline json error(const json& id, int code, const std::string& message)
{
    return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}
inline json handle_rpc(const json& request, const std::function<json(const std::string&, const json&)>& inspect)
{
    json id = nullptr;
    try {
        if (!request.is_object()) return error(id, -32600, "Invalid Request");
        if (request.contains("id")) {
            id = request["id"];
            if (!id.is_string() && !id.is_number_integer())
                return error(nullptr, -32600, "Invalid request id");
        }
        if (!request.contains("jsonrpc") || request["jsonrpc"] != "2.0" ||
            !request.contains("method") || !request["method"].is_string())
            return error(id, -32600, "Invalid Request");
        const std::string method = request["method"].get<std::string>();
        if (!request.contains("id")) {
            // Notifications never receive JSON-RPC responses. This stateless
            // server has no notification-triggered work: discard them before
            // tool dispatch, including unknown methods or tools/call without id.
            return nullptr;
        }
        json result;
        if (method == "initialize") {
            if (!request.contains("params") || !request["params"].is_object())
                return error(id, -32602, "initialize requires parameters");
            const auto version = request["params"].value("protocolVersion", "");
            result = {{"protocolVersion", supported_version(version) ? version : "2025-06-18"},
                      {"capabilities", {{"tools", {{"listChanged", false}}}}},
                      {"serverInfo", {{"name", "qidi-studio-inspection"}, {"version", "0.1.0"}}},
                      {"instructions", "Experimental inspection-only QIDI Studio server. Source (AGPL v3): https://github.com/davidrd123/QIDIStudio"}};
        } else if (method == "ping") result = json::object();
        else if (method == "tools/list") result = list_tools();
        else if (method == "tools/call") {
            if (!request.contains("params") || !request["params"].is_object())
                return error(id, -32602, "Invalid tool parameters");
            const json& params = request["params"];
            const std::string name = params.value("name", "");
            const Tool* tool = find_tool(name);
            if (!tool) return error(id, -32602, "Unknown or disabled tool");
            const json arguments = params.value("arguments", json::object());
            validate_arguments(*tool, arguments);
            json payload = inspect(name, arguments);
            const bool failed = payload.contains("error") && !payload["error"].is_null();
            result = {{"content", json::array({{{"type", "text"}, {"text", payload.dump(2)}}})},
                      {"structuredContent", payload}, {"isError", failed}};
        } else return error(id, -32601, "Method not found");
        return {{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
    } catch (const json::exception&) { return error(id, -32602, "Invalid parameter type or value"); }
      catch (const std::invalid_argument& e) { return error(id, -32602, e.what()); }
      catch (const std::exception&) { return error(id, -32603, "Inspection request failed"); }
      catch (...) { return error(id, -32603, "Internal error"); }
}
} // namespace Slic3r::GUI::McpProtocol
#endif
