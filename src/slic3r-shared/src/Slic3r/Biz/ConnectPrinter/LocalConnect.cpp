#include "Slic3r/Biz/ConnectPrinter/LocalConnect.hpp"

#include "Slic3r/Biz/Network/IHttp.hpp"
#include "Slic3r/Directories.hpp"
#include "Slic3r/Log.hpp"

#include <nlohmann/json.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include <cstdlib>
#include <cstring>
#include <memory>

namespace Slic3r::Biz::ConnectPrinter {

namespace {

/// Namespace of the Materials module of the local Connect, under /api/v1/modules/.
constexpr const char* MATERIALS_MODULE = "com.github.aux.prusa3d.materials";

std::string env(const char* name)
{
    const char* value = std::getenv(name);
    return value == nullptr ? std::string{} : std::string{value};
}

std::string string_at(const nlohmann::json& j, const char* key)
{
    if (j.is_object()) {
        if (auto it = j.find(key); it != j.end() && it->is_string()) {
            return it->get<std::string>();
        }
    }
    return {};
}

/// "#RRGGBBAA" or "#RRGGBB" to "#RRGGBB", anything else to empty.
std::string rgb_color(std::string color)
{
    if (color.size() == 9 && color.front() == '#') {
        color.resize(7);
    }
    return color.size() == 7 && color.front() == '#' ? color : std::string{};
}

/// Local Connect spells models "CoreOne_INDX", the cloud "COREONE_INDX".
std::string cloud_model(const std::string& local_model)
{
    return boost::to_upper_copy(local_model);
}

/// The family the cloud puts in base_model: a CORE One L is a CORE One there.
std::string cloud_base_model(const std::string& model)
{
    if (model == "COREONEL") {
        return "COREONE";
    }
    if (model == "COREONEL_INDX") {
        return "COREONE_INDX";
    }
    return model;
}

nlohmann::json material_json(const LocalToolMaterial& material)
{
    nlohmann::json mat = {{"class", "FFF"}};
    mat["type"]         = material.type.empty() ? material.abbreviation : material.type;
    mat["abbreviation"] = material.abbreviation.empty() ? material.type : material.abbreviation;
    if (!material.name.empty()) {
        mat["name"] = material.name;
    }
    if (!material.brand.empty()) {
        mat["brand"] = material.brand;
    }
    if (!material.material_uuid.empty()) {
        mat["uuid"] = material.material_uuid;
    }
    if (!material.color.empty()) {
        mat["color_rgba"] = material.color;
    }
    return mat;
}

/// The material the Materials module knows of a tool: the assignment, else what the printer reports.
std::optional<LocalToolMaterial> module_tool_material(const nlohmann::json& tool)
{
    LocalToolMaterial result;
    if (auto it = tool.find("assignment"); it != tool.end() && it->is_object()) {
        const nlohmann::json& a = *it;
        result.type          = string_at(a, "type");
        result.abbreviation  = string_at(a, "abbreviation");
        result.name          = string_at(a, "material_name");
        result.brand         = string_at(a, "brand_name");
        result.material_uuid = string_at(a, "material_uuid");
        result.color         = rgb_color(string_at(a, "color_rgba"));
    }
    if (result.type.empty() && result.abbreviation.empty()) {
        if (auto it = tool.find("reported"); it != tool.end() && it->is_object()) {
            result.type         = string_at(*it, "type");
            result.abbreviation = string_at(*it, "abbreviation");
            if (result.material_uuid.empty()) {
                result.material_uuid = string_at(*it, "material_uuid");
            }
            if (result.color.empty()) {
                result.color = rgb_color(string_at(*it, "color_rgba"));
            }
        }
    }
    if (result.type.empty() && result.abbreviation.empty()) {
        return std::nullopt;
    }
    return result;
}

/// The material type a device reports for a tool in its state, without the Materials module.
std::optional<LocalToolMaterial> reported_tool_material(const nlohmann::json& device, const std::string& tool_id)
{
    auto state = device.find("state");
    if (state == device.end() || !state->is_object()) {
        return std::nullopt;
    }
    const std::string key = "/tools/" + tool_id + "/material_package_instance/package/material/abbreviation";
    LocalToolMaterial result;
    result.abbreviation = string_at(*state, key.c_str());
    if (result.abbreviation.empty()) {
        return std::nullopt;
    }
    result.type = result.abbreviation;
    return result;
}

nlohmann::json printer_json(const nlohmann::json& device, const nlohmann::json* module_tools)
{
    const std::string model = cloud_model(string_at(device, "model"));
    nlohmann::json printer  = {
        {"uuid", string_at(device, "id")},
        {"name", string_at(device, "name")},
        {"model", model},
        {"base_model", cloud_base_model(model)},
        {"supported_printer_models", nlohmann::json::array()},
    };

    std::string state;
    if (auto it = device.find("connected"); it != device.end() && it->is_boolean() && !it->get<bool>()) {
        state = "OFFLINE";
    } else if (auto st = device.find("state"); st != device.end() && st->is_object()) {
        state = string_at(*st, "/state");
    }
    printer["connect_state"] = state.empty() ? "OFFLINE" : state;

    // Tools of the device: features from the core, materials from the module.
    std::map<std::string, nlohmann::json> module_by_id;
    if (module_tools != nullptr) {
        if (auto it = module_tools->find("tools"); it != module_tools->end() && it->is_array()) {
            for (const nlohmann::json& tool : *it) {
                module_by_id[string_at(tool, "id")] = tool;
            }
        }
    }

    nlohmann::json tools = nlohmann::json::object();
    const nlohmann::json* core_tools = nullptr;
    if (auto it = device.find("tools"); it != device.end() && it->is_object()) {
        if (auto inner = it->find("tools"); inner != it->end() && inner->is_object()) {
            core_tools = &*inner;
        }
    }
    if (core_tools != nullptr) {
        for (const auto& [id, core_tool] : core_tools->items()) {
            nlohmann::json tool = {{"class", "FFF"}, {"type", "PRINT_HEAD"}};
            if (auto f = core_tool.find("features"); f != core_tool.end() && f->is_object()) {
                tool["features"] = *f;
            } else {
                tool["features"] = nlohmann::json::object();
            }
            std::optional<LocalToolMaterial> material;
            if (auto m = module_by_id.find(id); m != module_by_id.end()) {
                material = module_tool_material(m->second);
            } else {
                material = reported_tool_material(device, id);
            }
            if (material.has_value()) {
                tool["material_package_instance"] = {
                    {"class", "FFF"},
                    {"is_from_nfc", false},
                    {"package", {{"class", "FFF"}, {"material", material_json(*material)}}},
                };
            }
            tools[id] = std::move(tool);
        }
    }
    printer["tool_count"] = tools.size();
    printer["tools"]      = std::move(tools);
    return printer;
}

struct Response
{
    bool ok{false};
    unsigned status{0};
    std::string body;
    std::string error;
    std::string headers;
};

Response perform(Network::IHttp::RequestMethod method, const std::string& url, const std::string& auth_header,
                 const std::string& auth_value, const std::string* post_body)
{
    Response response;
    std::unique_ptr<Network::IHttp> http =
        Network::IHttp::create(method, url, [](Network::IHttp::Retry, bool&) {});
    http->timeout_connection(5);
    http->timeout_total(20);
    if (!auth_header.empty()) {
        http->header(auth_header, auth_value);
    }
    if (post_body != nullptr) {
        http->header("Content-Type", "application/json");
        http->set_post_body(*post_body);
    }
    http->on_headers_read([&response](const std::string& headers) { response.headers += headers; });
    http->on_complete(
        [&response](std::string body, unsigned status)
        {
            response.ok     = true;
            response.status = status;
            response.body   = std::move(body);
        }
    );
    http->on_error(
        [&response](std::string body, std::string error, unsigned status)
        {
            response.ok     = false;
            response.status = status;
            response.body   = std::move(body);
            response.error  = std::move(error);
        }
    );
    http->perform_sync();
    return response;
}

/// The session cookie a login answer sets, as "name=value".
std::string session_cookie(const std::string& headers)
{
    std::vector<std::string> lines;
    boost::split(lines, headers, boost::is_any_of("\n"));
    for (std::string line : lines) {
        boost::trim(line);
        if (!boost::istarts_with(line, "set-cookie:")) {
            continue;
        }
        std::string value = boost::trim_copy(line.substr(std::strlen("set-cookie:")));
        if (size_t semicolon = value.find(';'); semicolon != std::string::npos) {
            value.resize(semicolon);
        }
        if (!value.empty()) {
            return value;
        }
    }
    return {};
}

} // namespace

LocalConnectSettings LocalConnectSettings::load(const std::string& json_path)
{
    LocalConnectSettings settings;
    if (!json_path.empty() && boost::filesystem::exists(json_path)) {
        try {
            boost::nowide::ifstream file(json_path);
            nlohmann::json json = nlohmann::json::parse(file);
            settings.url      = string_at(json, "url");
            settings.token    = string_at(json, "token");
            settings.username = string_at(json, "username");
            settings.password = string_at(json, "password");
        } catch (const std::exception& e) {
            SPDLOG_ERROR("Cannot read local Connect settings {}: {}", json_path, e.what());
        }
    }
    if (std::string v = env("PRUSA_LOCAL_CONNECT_URL"); !v.empty()) {
        settings.url = v;
    }
    if (std::string v = env("PRUSA_LOCAL_CONNECT_TOKEN"); !v.empty()) {
        settings.token = v;
    }
    if (std::string v = env("PRUSA_LOCAL_CONNECT_USERNAME"); !v.empty()) {
        settings.username = v;
    }
    if (std::string v = env("PRUSA_LOCAL_CONNECT_PASSWORD"); !v.empty()) {
        settings.password = v;
    }
    while (!settings.url.empty() && settings.url.back() == '/') {
        settings.url.pop_back();
    }
    return settings;
}

const LocalConnectSettings& LocalConnectSettings::instance()
{
    static const LocalConnectSettings settings = []
    {
        LocalConnectSettings s = load((boost::filesystem::path(data_dir()) / "local_connect.json").string());
        if (s.enabled()) {
            SPDLOG_INFO("Printers come from the local Connect at {}", s.url);
        }
        return s;
    }();
    return settings;
}

std::string local_connect_printer_list(
    const nlohmann::json& devices,
    const std::map<std::string, nlohmann::json>& tools
)
{
    nlohmann::json printers = nlohmann::json::array();
    if (auto items = devices.find("items"); items != devices.end() && items->is_array()) {
        for (const nlohmann::json& device : *items) {
            if (string_at(device, "kind") != "printer" || string_at(device, "id").empty()) {
                continue;
            }
            // Only Prusa printers have profiles the cloud model names resolve to.
            if (const std::string vendor = string_at(device, "vendor");
                !vendor.empty() && !boost::iequals(vendor, "Prusa"))
            {
                continue;
            }
            auto it = tools.find(string_at(device, "id"));
            nlohmann::json printer = printer_json(device, it == tools.end() ? nullptr : &it->second);
            // A device that reports no tools cannot be matched to a printer profile.
            if (printer["tools"].empty()) {
                continue;
            }
            printers.push_back(std::move(printer));
        }
    }
    return nlohmann::json{{"printers", std::move(printers)}}.dump();
}

std::optional<LocalToolMaterial> local_tool_material(const nlohmann::json& tool_json)
{
    try {
        const nlohmann::json& mat = tool_json.at("material_package_instance").at("package").at("material");
        LocalToolMaterial result;
        result.type          = string_at(mat, "type");
        result.abbreviation  = string_at(mat, "abbreviation");
        result.name          = string_at(mat, "name");
        result.brand         = string_at(mat, "brand");
        result.material_uuid = string_at(mat, "uuid");
        result.color         = rgb_color(string_at(mat, "color_rgba"));
        return result;
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
}

std::optional<std::string> fetch_local_connect_printer_list(
    const LocalConnectSettings& settings,
    std::string& error
)
{
    using Method = Network::IHttp::RequestMethod;
    const std::string api = settings.url + "/api/v1";

    std::string auth_header;
    std::string auth_value;
    if (!settings.token.empty()) {
        auth_header = "Authorization";
        auth_value  = "Bearer " + settings.token;
    } else if (!settings.username.empty()) {
        const std::string body =
            nlohmann::json{{"username", settings.username}, {"password", settings.password}}.dump();
        Response login = perform(Method::Post, api + "/auth/login", {}, {}, &body);
        std::string cookie = login.ok ? session_cookie(login.headers) : std::string{};
        if (cookie.empty()) {
            error = "Login to the local Connect failed (" + std::to_string(login.status) + ") " + login.error;
            return std::nullopt;
        }
        auth_header = "Cookie";
        auth_value  = cookie;
    }

    Response devices_response = perform(Method::Get, api + "/devices", auth_header, auth_value, nullptr);
    if (!devices_response.ok) {
        error = "Reading printers from the local Connect failed (" + std::to_string(devices_response.status)
            + ") " + devices_response.error;
        return std::nullopt;
    }

    nlohmann::json devices;
    try {
        devices = nlohmann::json::parse(devices_response.body);
    } catch (const nlohmann::json::exception& e) {
        error = std::string("The local Connect answered printers that cannot be read: ") + e.what();
        return std::nullopt;
    }

    std::map<std::string, nlohmann::json> tools;
    if (auto items = devices.find("items"); items != devices.end() && items->is_array()) {
        for (const nlohmann::json& device : *items) {
            const std::string id = string_at(device, "id");
            if (id.empty() || string_at(device, "kind") != "printer") {
                continue;
            }
            const std::string url = api + "/modules/" + MATERIALS_MODULE + "/printers/" + id + "/tools";
            Response tools_response = perform(Method::Get, url, auth_header, auth_value, nullptr);
            if (!tools_response.ok) {
                // Without the Materials module the printer reports material types only.
                SPDLOG_INFO("No tools from the local Connect Materials module for {} ({})", id, tools_response.status);
                continue;
            }
            try {
                tools[id] = nlohmann::json::parse(tools_response.body);
            } catch (const nlohmann::json::exception& e) {
                SPDLOG_WARN("Cannot read the tools of {} from the local Connect: {}", id, e.what());
            }
        }
    }
    return local_connect_printer_list(devices, tools);
}

} // namespace Slic3r::Biz::ConnectPrinter
