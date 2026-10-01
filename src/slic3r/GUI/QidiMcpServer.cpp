// SPDX-License-Identifier: AGPL-3.0-only
// Portable inspection-only MCP integration, adapted from sslinkyy's QIDI Studio
// MCP work: https://github.com/sslinkyy/QIDIStudio (QidiMcpServer.cpp, v1.11.0,
// commit d85c905619f6637eed24b6967a4a8659ab3641bf).
// The project/plate serializers and GUI dispatch pattern retain that authorship.
// Modified 2026-09-30 for authenticated portable inspection and bounded teardown.
// This version deliberately exposes no mutations, network printer access,
// screenshots, file import/export, or tunnel management.
#include "QidiMcpServer.hpp"
#include "QidiMcpProtocol.hpp"
#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "Plater.hpp"
#include "PartPlate.hpp"
#include "Tab.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"

#include <boost/asio.hpp>
#include <boost/log/trivial.hpp>
#include <wx/dialog.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <mutex>
#include <thread>

namespace Slic3r::GUI {
namespace {
using McpProtocol::json;
using tcp = boost::asio::ip::tcp;
using Clock = std::chrono::steady_clock;

std::string utf8(const wxString& value)
{
    const auto bytes = value.ToUTF8();
    return bytes.data() ? std::string(bytes.data(), bytes.length()) : std::string();
}
std::string environment(const char* key)
{
    const char* value = std::getenv(key);
    return value ? std::string(value) : std::string();
}
bool valid_token(const std::string& token)
{
    return !token.empty() && token.size() <= 4096 &&
           std::all_of(token.begin(), token.end(), [](unsigned char c) { return c >= 33 && c <= 126; });
}
bool modal_is_open()
{
    for (auto node = wxTopLevelWindows.GetFirst(); node; node = node->GetNext()) {
        auto* dialog = dynamic_cast<wxDialog*>(node->GetData());
        if (dialog && dialog->IsShown() && dialog->IsModal()) return true;
    }
    return false;
}
Plater* require_plater()
{
    Plater* plater = wxGetApp().plater();
    if (!plater) throw std::runtime_error("Plater is not available");
    if (modal_is_open()) throw std::runtime_error("Resolve the open modal dialog before inspecting the project");
    return plater;
}
PartPlate* require_plate(Plater* plater)
{
    auto& plates = plater->get_partplate_list();
    const int index = plates.get_curr_plate_index();
    if (index < 0 || index >= plates.get_plate_count()) throw std::runtime_error("No active plate");
    PartPlate* plate = plates.get_curr_plate();
    if (!plate) throw std::runtime_error("No active plate");
    return plate;
}
ModelObject* require_object(Plater* plater, int id)
{
    const auto& objects = plater->model().objects;
    if (id < 0 || static_cast<std::size_t>(id) >= objects.size() || !objects[id])
        throw std::runtime_error("object_id is out of range");
    return objects[id];
}
json vec3(const Vec3d& value)
{
    return {{"x", value.x()}, {"y", value.y()}, {"z", value.z()}};
}
json bounds(const BoundingBoxf3& box)
{
    if (!box.defined) return nullptr;
    return {{"min_mm", vec3(box.min)}, {"max_mm", vec3(box.max)}, {"size_mm", vec3(box.size())}};
}
json instance_state(ModelInstance* instance, std::size_t id)
{
    return {{"instance_id", id}, {"printable", instance->is_printable()},
            {"position_mm", vec3(instance->get_offset())}, {"rotation_rad", vec3(instance->get_rotation())},
            {"scale", vec3(instance->get_scaling_factor())}, {"mirror", vec3(instance->get_mirror())}};
}
json object_state(ModelObject* object, std::size_t id)
{
    json instances = json::array();
    for (std::size_t i = 0; i < object->instances.size(); ++i) {
        if (!object->instances[i]) continue;
        json state = instance_state(object->instances[i], i);
        state["bounding_box"] = bounds(object->instance_bounding_box(i));
        instances.push_back(std::move(state));
    }
    // Report live per-instance bounds without explicitly invalidating model caches.
    return {{"object_id", id}, {"name", object->name}, {"printable", object->printable},
            {"instance_count", object->instances.size()}, {"volume_count", object->volumes.size()},
            {"facet_count", object->facets_count()}, {"instances", std::move(instances)}};
}
std::string serialize_option(const std::string& key, const ConfigOption* option)
{
    if (!option) throw std::runtime_error("Unknown setting: " + key);
    const ConfigOptionDef* definition = print_config_def.get(key);
    const auto* keys = definition ? definition->enum_keys_map : nullptr;
    // Some dynamic enum options lack the map. Repair a temporary copy only.
    if (const auto* value = dynamic_cast<const ConfigOptionEnumGeneric*>(option); value && !value->keys_map) {
        if (!keys) throw std::runtime_error("Enum setting has no serialization map: " + key);
        ConfigOptionEnumGeneric copy(*value); copy.keys_map = keys; return copy.serialize();
    }
    if (const auto* value = dynamic_cast<const ConfigOptionEnumsGeneric*>(option); value && !value->keys_map) {
        if (!keys) throw std::runtime_error("Enum setting has no serialization map: " + key);
        ConfigOptionEnumsGeneric copy(*value); copy.keys_map = keys; return copy.serialize();
    }
    if (const auto* value = dynamic_cast<const ConfigOptionEnumsGenericNullable*>(option); value && !value->keys_map) {
        if (!keys) throw std::runtime_error("Enum setting has no serialization map: " + key);
        ConfigOptionEnumsGenericNullable copy(*value); copy.keys_map = keys; return copy.serialize();
    }
    return option->serialize();
}
json inspect_gui(const std::string& tool, const json& arguments)
{
    if (!wxIsMainThread()) throw std::runtime_error("Inspection must run on the GUI thread");
    if (wxGetApp().is_recreating_gui()) throw std::runtime_error("Studio is rebuilding its interface; retry shortly");
    if (tool == "get_ui_state") {
        json dialogs = json::array();
        bool blocked = false;
        for (auto node = wxTopLevelWindows.GetFirst(); node; node = node->GetNext()) {
            auto* dialog = dynamic_cast<wxDialog*>(node->GetData());
            if (!dialog || !dialog->IsShown()) continue;
            blocked = blocked || dialog->IsModal();
            dialogs.push_back({{"title", utf8(dialog->GetTitle())}, {"modal", dialog->IsModal()}});
        }
        MainFrame* frame = wxGetApp().mainframe;
        const int selected = frame && frame->m_tabpanel ? frame->m_tabpanel->GetSelection() : -1;
        return {{"ui_blocked", blocked}, {"selected_tab", selected},
                {"selected_view", selected == MainFrame::tp3DEditor ? "prepare" :
                                  selected == MainFrame::tpPreview ? "preview" : "other"},
                {"dialogs", std::move(dialogs)}};
    }
    Plater* plater = require_plater();
    auto& plates = plater->get_partplate_list();
    if (tool == "get_project_state") return {
        {"name", utf8(plater->get_project_name())}, {"filename", utf8(plater->get_project_filename())},
        {"project_dirty", plater->is_project_dirty()}, {"presets_dirty", plater->is_presets_dirty()},
        {"object_count", plater->model().objects.size()}, {"plate_count", plates.get_plate_count()},
        {"active_plate", plates.get_curr_plate_index()}
    };
    if (tool == "list_plates") {
        json items = json::array();
        for (PartPlate* plate : plates.get_plate_list()) if (plate) items.push_back({
            {"index", plate->get_index()}, {"name", plate->get_plate_name()},
            {"active", plate->get_index() == plates.get_curr_plate_index()}, {"empty", plate->empty()},
            {"locked", plate->is_locked()}, {"can_slice", plate->can_slice()},
            {"slice_valid", plate->is_slice_result_valid()}, {"instance_count", plate->get_obj_and_inst_set().size()}
        });
        return {{"plates", std::move(items)}, {"count", plates.get_plate_count()}};
    }
    if (tool == "list_objects") {
        json items = json::array();
        const auto& objects = plater->model().objects;
        for (std::size_t i = 0; i < objects.size(); ++i) if (objects[i]) items.push_back(object_state(objects[i], i));
        return {{"objects", std::move(items)}};
    }
    if (tool == "get_object_state") {
        const int id = arguments["object_id"].get<int>();
        return {{"object", object_state(require_object(plater, id), id)}};
    }
    if (tool == "list_object_volumes") {
        const int id = arguments["object_id"].get<int>();
        ModelObject* object = require_object(plater, id);
        json volumes = json::array();
        for (std::size_t i = 0; i < object->volumes.size(); ++i) {
            ModelVolume* volume = object->volumes[i];
            if (!volume) continue;
            const bool inherits = !volume->config.has("extruder");
            const int extruder = inherits ? (object->config.has("extruder") ? object->config.extruder() : 1)
                                         : volume->config.extruder();
            volumes.push_back({{"volume_id", i}, {"name", volume->name},
                {"type", ModelVolume::type_to_string(volume->type())}, {"extruder", extruder},
                {"inherits_object_extruder", inherits}, {"facet_count", volume->mesh().facets_count()},
                {"position_mm", vec3(volume->get_offset())}, {"rotation_rad", vec3(volume->get_rotation())},
                {"scale", vec3(volume->get_scaling_factor())}});
        }
        return {{"object_id", id}, {"volumes", std::move(volumes)}};
    }
    if (tool == "get_active_presets" || tool == "get_slice_settings") {
        PresetBundle* bundle = wxGetApp().preset_bundle;
        if (!bundle) throw std::runtime_error("Preset bundle is not available");
        if (tool == "get_active_presets") return {
            {"active", {{"printer", bundle->printers.get_edited_preset().name},
                        {"print", bundle->prints.get_edited_preset().name}, {"filaments", bundle->filament_presets}}},
            {"edited_dirty", {{"printer", bundle->printers.get_edited_preset().is_dirty},
                              {"print", bundle->prints.get_edited_preset().is_dirty},
                              {"selected_filament", bundle->filaments.get_edited_preset().is_dirty}}}
        };
        const std::string scope = arguments.value("scope", "print");
        const DynamicPrintConfig* config = nullptr;
        std::string preset_name;
        int index = -1;
        if (scope == "filament") {
            const auto count = bundle->filament_presets.size();
            if (count == 0) throw std::runtime_error("Project has no assigned filaments");
            if (!arguments.contains("filament_index") && count != 1)
                throw std::runtime_error("Provide filament_index when the project has multiple filaments");
            index = arguments.value("filament_index", 0);
            if (static_cast<std::size_t>(index) >= count) throw std::runtime_error("filament_index is out of range");
            preset_name = bundle->filament_presets[index];
            // find_preset(real=false) supplies the edited copy when this preset is selected.
            const Preset* preset = bundle->filaments.find_preset(preset_name, false);
            if (!preset) throw std::runtime_error("Assigned filament preset is not available");
            config = &preset->config;
        } else {
            Tab* tab = wxGetApp().get_tab(scope == "print" ? Preset::TYPE_PRINT : Preset::TYPE_PRINTER);
            if (!tab || !tab->get_config()) throw std::runtime_error("Settings tab is not available");
            config = tab->get_config();
            preset_name = scope == "print" ? bundle->prints.get_edited_preset().name : bundle->printers.get_edited_preset().name;
        }
        json values = json::object();
        for (const json& key : arguments["keys"]) {
            const std::string name = key.get<std::string>();
            values[name] = serialize_option(name, config->option(name));
        }
        return {{"scope", scope}, {"preset", preset_name}, {"filament_index", index >= 0 ? json(index) : json(nullptr)},
                {"source", scope == "filament" ? "assigned_preset_with_current_edits" : "edited_tab_config"},
                {"values", std::move(values)}};
    }
    PartPlate* plate = require_plate(plater);
    if (tool == "get_plate_state") {
        json instances = json::array();
        for (const auto& item : plate->get_obj_and_inst_set()) {
            ModelInstance* instance = plate->get_instance(item.first, item.second);
            if (!instance) continue;
            json state = instance_state(instance, item.second);
            state["object_id"] = item.first;
            instances.push_back(std::move(state));
        }
        const auto size = plate->get_size();
        return {{"plate", {{"index", plate->get_index()}, {"name", plate->get_plate_name()},
                          {"origin_mm", vec3(plate->get_origin())}, {"size_mm", {{"x", size.x()}, {"y", size.y()}}},
                          {"instances", std::move(instances)}}}};
    }
    if (tool == "get_slice_status") return {
        {"plate_index", plate->get_index()}, {"slicing", plater->is_background_process_slicing()},
        {"job_running", plater->is_any_job_running()}, {"progress_percent", plate->get_slicing_percent()},
        {"valid", plate->is_slice_result_valid()}, {"ready_for_print", plate->is_slice_result_ready_for_print()},
        {"can_slice", plate->can_slice()}
    };
    if (tool == "get_slice_result") {
        if (plater->is_background_process_slicing() || plater->is_any_job_running())
            throw std::runtime_error("Wait for the current Studio job to finish");
        if (!plate->is_slice_result_valid()) throw std::runtime_error("The active plate has no valid slice result");
        auto* result = plate->get_slice_result();
        if (!result) throw std::runtime_error("Slice result is unavailable");
        std::unique_lock<std::mutex> lock(result->result_mutex, std::try_to_lock);
        if (!lock.owns_lock()) throw std::runtime_error("Slice result is busy; retry shortly");
        const auto& mode = result->print_statistics.modes[static_cast<std::size_t>(PrintEstimatedStatistics::ETimeMode::Normal)];
        json warnings = json::array();
        for (const auto& warning : result->warnings) warnings.push_back({{"level", warning.level}, {"message", warning.msg}});
        return {{"plate_index", plate->get_index()}, {"gcode_file", result->filename},
                {"normal_time_s", mode.time}, {"normal_prepare_time_s", mode.prepare_time},
                {"layer_count", mode.layers_times.size()}, {"warnings", std::move(warnings)}};
    }
    throw std::runtime_error("No inspection handler for this tool");
}

struct Lifetime { std::atomic<bool> stopping{false}; };
json on_gui_thread(const std::shared_ptr<Lifetime>& lifetime, const std::string& name, const json& args)
{
    auto promise = std::make_shared<std::promise<json>>();
    auto future = promise->get_future();
    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    wxGetApp().CallAfter([lifetime, promise, cancelled, name, args]() {
        if (lifetime->stopping || *cancelled) return;
        try { promise->set_value(inspect_gui(name, args)); }
        catch (const std::exception& e) { promise->set_value({{"error", e.what()}}); }
        catch (...) { promise->set_value({{"error", "GUI inspection failed"}}); }
    });
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    while (!lifetime->stopping && Clock::now() < deadline) {
        if (future.wait_for(std::chrono::milliseconds(20)) == std::future_status::ready) return future.get();
    }
    *cancelled = true;
    return {{"error", lifetime->stopping ? "MCP server is stopping" : "GUI inspection timed out"},
            {"error_code", lifetime->stopping ? "STOPPING" : "GUI_TIMEOUT"}};
}
const char* reason(int status)
{
    switch (status) {
    case 200: return "OK"; case 202: return "Accepted"; case 204: return "No Content";
    case 401: return "Unauthorized"; case 403: return "Forbidden"; case 404: return "Not Found";
    case 405: return "Method Not Allowed"; case 408: return "Request Timeout";
    case 413: return "Content Too Large"; case 415: return "Unsupported Media Type";
    default: return "Bad Request";
    }
}
void write_response(tcp::socket& socket, const std::shared_ptr<Lifetime>& lifetime,
                    int status, const std::string& body, const std::string& origin = {})
{
    std::ostringstream header;
    header << "HTTP/1.1 " << status << ' ' << reason(status) << "\r\nContent-Type: application/json\r\n"
           << "Content-Length: " << body.size() << "\r\nConnection: close\r\nCache-Control: no-store\r\n";
    if (status == 401) header << "WWW-Authenticate: Bearer\r\n";
    if (!origin.empty() && McpProtocol::local_origin(origin))
        header << "Access-Control-Allow-Origin: " << origin << "\r\nVary: Origin\r\n"
               << "Access-Control-Allow-Methods: POST, OPTIONS\r\n"
               << "Access-Control-Allow-Headers: Authorization, Content-Type, Accept, MCP-Protocol-Version\r\n";
    const std::string bytes = header.str() + "\r\n" + body;
    std::size_t offset = 0;
    const auto deadline = Clock::now() + std::chrono::seconds(2);
    while (!lifetime->stopping && offset < bytes.size() && Clock::now() < deadline) {
        boost::system::error_code ec;
        const auto count = socket.write_some(boost::asio::buffer(bytes.data() + offset, bytes.size() - offset), ec);
        if (!ec) { offset += count; continue; }
        if (ec != boost::asio::error::would_block && ec != boost::asio::error::try_again) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}
McpProtocol::HttpRequest read_request(tcp::socket& socket, const std::shared_ptr<Lifetime>& lifetime,
                                     const std::string& token)
{
    std::string bytes;
    std::array<char, 4096> buffer;
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    std::size_t body_offset = 0;
    McpProtocol::HttpRequest request;
    while (!lifetime->stopping && Clock::now() < deadline) {
        boost::system::error_code ec;
        const auto count = socket.read_some(boost::asio::buffer(buffer), ec);
        if (!ec) {
            bytes.append(buffer.data(), count);
            if (bytes.size() > McpProtocol::max_request_bytes) throw McpProtocol::HttpError(413, "Request is too large");
            if (body_offset == 0) {
                const auto end = bytes.find("\r\n\r\n");
                if (end != std::string::npos) {
                    body_offset = end + 4;
                    request = McpProtocol::parse_headers(bytes.substr(0, body_offset));
                    McpProtocol::validate_http(request, token);
                    if (request.content_length > McpProtocol::max_request_bytes - body_offset)
                        throw McpProtocol::HttpError(413, "Request is too large");
                }
            }
            if (body_offset != 0 && bytes.size() - body_offset >= request.content_length) {
                request.body = bytes.substr(body_offset, request.content_length);
                return request;
            }
            continue;
        }
        if (ec != boost::asio::error::would_block && ec != boost::asio::error::try_again)
            throw McpProtocol::HttpError(400, "Incomplete HTTP request");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    throw McpProtocol::HttpError(408, "Request read timed out");
}
} // namespace

struct QidiMcpServer::Impl {
    boost::asio::io_context io;
    tcp::acceptor acceptor{io};
    std::shared_ptr<Lifetime> lifetime = std::make_shared<Lifetime>();
    std::thread thread;
    std::string token;

    bool start()
    {
        if (thread.joinable()) return true;
        token = environment("QIDI_STUDIO_MCP_TOKEN");
        if (environment("QIDI_STUDIO_MCP") != "1" || !valid_token(token)) return false;
        const tcp::endpoint endpoint(boost::asio::ip::address_v4::loopback(), McpProtocol::port);
        boost::system::error_code ec;
        acceptor.open(endpoint.protocol(), ec);
        if (!ec) acceptor.bind(endpoint, ec);
        if (!ec) acceptor.listen(8, ec);
        if (!ec) acceptor.non_blocking(true, ec);
        if (ec) {
            BOOST_LOG_TRIVIAL(error) << "MCP inspection server could not bind local port " << McpProtocol::port << ": " << ec.message();
            boost::system::error_code ignored; acceptor.close(ignored); token.clear(); return false;
        }
        lifetime = std::make_shared<Lifetime>();
        thread = std::thread([this] { run(); });
        BOOST_LOG_TRIVIAL(info) << "MCP inspection server listening on http://127.0.0.1:8765/mcp";
        return true;
    }
    void stop()
    {
        // The worker owns socket/acceptor operations. Nonblocking I/O and short
        // cancellation polls let GUI-thread teardown join without waiting on GUI work.
        lifetime->stopping = true;
        if (thread.joinable()) thread.join();
        boost::system::error_code ignored; acceptor.close(ignored); token.clear();
    }
    void serve(tcp::socket& socket)
    {
        try {
            const auto request = read_request(socket, lifetime, token);
            const std::string origin = McpProtocol::header(request, "origin");
            if (request.method == "OPTIONS") { write_response(socket, lifetime, 204, {}, origin); return; }
            json rpc;
            try { rpc = json::parse(request.body); }
            catch (const json::exception&) {
                write_response(socket, lifetime, 200, McpProtocol::error(nullptr, -32700, "Parse error").dump(), origin);
                return;
            }
            const json response = McpProtocol::handle_rpc(rpc, [this](const std::string& name, const json& args) {
                return on_gui_thread(lifetime, name, args);
            });
            write_response(socket, lifetime, response.is_null() ? 202 : 200,
                           response.is_null() ? std::string() : response.dump(), origin);
        } catch (const McpProtocol::HttpError& e) {
            write_response(socket, lifetime, e.status, json{{"error", e.what()}}.dump());
        } catch (const std::exception&) {
            write_response(socket, lifetime, 400, "{\"error\":\"Request failed\"}");
        } catch (...) {
            write_response(socket, lifetime, 400, "{\"error\":\"Request failed\"}");
        }
    }
    void run()
    {
        try {
            while (!lifetime->stopping) {
                tcp::socket socket(io);
                boost::system::error_code ec;
                acceptor.accept(socket, ec);
                if (ec == boost::asio::error::would_block || ec == boost::asio::error::try_again) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10)); continue;
                }
                if (ec) break;
                socket.non_blocking(true, ec);
                if (!ec) serve(socket);
            }
        } catch (const std::exception&) {
            BOOST_LOG_TRIVIAL(error) << "MCP inspection worker stopped after an unexpected error";
        } catch (...) {
            BOOST_LOG_TRIVIAL(error) << "MCP inspection worker stopped after an unexpected error";
        }
    }
};

QidiMcpServer::QidiMcpServer() : m_impl(std::make_unique<Impl>()) {}
QidiMcpServer::~QidiMcpServer() { stop(); }
bool QidiMcpServer::enabled_by_environment()
{
    return environment("QIDI_STUDIO_MCP") == "1" && valid_token(environment("QIDI_STUDIO_MCP_TOKEN"));
}
bool QidiMcpServer::start() { return m_impl->start(); }
void QidiMcpServer::stop() { if (m_impl) m_impl->stop(); }
} // namespace Slic3r::GUI
