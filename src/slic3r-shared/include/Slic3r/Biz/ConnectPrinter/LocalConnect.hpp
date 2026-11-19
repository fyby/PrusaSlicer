#pragma once

#include <map>
#include <optional>
#include <string>

#include <nlohmann/json_fwd.hpp>

namespace Slic3r::Biz::ConnectPrinter {

/**
 * @brief Where a self-hosted (local) Prusa Connect runs and how to sign in to it.
 *
 * Read from the environment first (PRUSA_LOCAL_CONNECT_URL, PRUSA_LOCAL_CONNECT_TOKEN,
 * PRUSA_LOCAL_CONNECT_USERNAME, PRUSA_LOCAL_CONNECT_PASSWORD), then from
 * `<data_dir>/local_connect.json` with the keys url, token, username and password.
 * With an url set, the Connect printer list comes from that server instead of the cloud,
 * and no Prusa Account login is needed to pick its printers.
 */
struct LocalConnectSettings
{
    std::string url;
    std::string token;
    std::string username;
    std::string password;

    bool enabled() const { return !url.empty(); }

    /// The settings of this run, read once.
    static const LocalConnectSettings& instance();

    /// Reads the environment and the given json file (missing file is not an error).
    static LocalConnectSettings load(const std::string& json_path);
};

/// The material assigned to a tool by the Materials module of the local Connect.
struct LocalToolMaterial
{
    std::string type;
    std::string abbreviation;
    std::string name;
    std::string brand;
    std::string material_uuid;
    std::string color; ///< "#RRGGBB", empty when unknown
};

/**
 * @brief Builds the printer list in the shape the cloud endpoint answers, from the answers of
 *        the local Connect.
 *
 * @param devices Answer of GET /api/v1/devices.
 * @param tools   Answer of the Materials module's GET /printers/{device}/tools, per device id.
 *                A device without an entry takes the material type its tools report.
 * @return Json text with a "printers" array, which parse_connect_printer_list() reads.
 */
std::string local_connect_printer_list(
    const nlohmann::json& devices,
    const std::map<std::string, nlohmann::json>& tools
);

/// The material of one tool of the printer json local_connect_printer_list() wrote.
std::optional<LocalToolMaterial> local_tool_material(const nlohmann::json& tool_json);

/**
 * @brief Reads the printers and their tools from the local Connect. Blocking, call it off the
 *        main thread.
 * @param[out] error Why it failed, when it returns nullopt.
 * @return The printer list, see local_connect_printer_list().
 */
std::optional<std::string> fetch_local_connect_printer_list(
    const LocalConnectSettings& settings,
    std::string& error
);

} // namespace Slic3r::Biz::ConnectPrinter
