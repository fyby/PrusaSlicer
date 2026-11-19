#include "Slic3r/Biz/ConnectPrinter/ConnectPrinter.hpp"
#include "Slic3r/Biz/ConnectPrinter/LocalConnect.hpp"
#include "Slic3r/Biz/PhysicalPrinter/PhysicalPrinterConfig.hpp"
#include "Slic3r/TestUtils/TestData.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include <cstdlib>

using namespace Slic3r;
using namespace Slic3r::Biz::ConnectPrinter;

namespace {
nlohmann::json read_local_connect_json(const std::string& file_name)
{
    auto path = Tests::get_datadir() / "local_connect" / file_name;
    boost::nowide::ifstream f{path.string()};
    REQUIRE(f.is_open());
    return nlohmann::json::parse(f);
}

constexpr const char* INDX_ID = "0439b159-a9a3-45fa-9371-b90d2f7500b2";
} // namespace

TEST_CASE("Local Connect printers read as the cloud printer list", "[LocalConnect]")
{
    const nlohmann::json devices = read_local_connect_json("devices.json");
    const nlohmann::json tools   = read_local_connect_json("tools.json");

    const std::string body = local_connect_printer_list(devices, {{INDX_ID, tools}});
    const auto printers    = parse_connect_printer_list(body);
    REQUIRE(printers.has_value());
    // The Klipper device is not a Prusa printer and is left out.
    REQUIRE(printers->size() == 1);

    const ConnectPrinter& printer = printers->front();
    CHECK(printer.uuid == INDX_ID);
    CHECK(printer.model == "COREONE_INDX");
    CHECK(printer.base_model == "COREONE_INDX");
    CHECK(printer.state != ConnectPrinterState::Unknown);

    SECTION("the hw config of the INDX loads with eight tools")
    {
        auto hw_config = Biz::PhysicalPrinter::hw_config_from_connect_json(printer.json);
        REQUIRE(hw_config.has_value());
        CHECK(hw_config->model.model == "COREONE_INDX");
        CHECK(hw_config->tool_count == 8);
    }

    SECTION("the spool assigned to tool 1 carries its material, brand, uuid and color")
    {
        const nlohmann::json json = nlohmann::json::parse(printer.json);
        auto material = local_tool_material(json["tools"]["1"]);
        REQUIRE(material.has_value());
        CHECK(material->type == "PETG");
        CHECK(material->name == "PETG Prusa Galaxy Black");
        CHECK(material->brand == "Prusament");
        CHECK(material->material_uuid == "0713a0d1-c6c9-5f3e-96c7-4c06ca543a6a");
        CHECK(material->color == "#3d3e3d");
    }

    SECTION("a tool without a spool carries the type the printer reports")
    {
        const nlohmann::json json = nlohmann::json::parse(printer.json);
        auto material = local_tool_material(json["tools"]["2"]);
        REQUIRE(material.has_value());
        CHECK(material->abbreviation == "ASA");
        CHECK(material->material_uuid.empty());
        CHECK(material->color.empty());
    }
}

TEST_CASE("Without the Materials module the printer's own material types are used", "[LocalConnect]")
{
    const nlohmann::json devices = read_local_connect_json("devices.json");
    const auto printers = parse_connect_printer_list(local_connect_printer_list(devices, {}));
    REQUIRE(printers.has_value());
    REQUIRE(printers->size() == 1);
    const nlohmann::json json = nlohmann::json::parse(printers->front().json);
    auto material = local_tool_material(json["tools"]["0"]);
    REQUIRE(material.has_value());
    CHECK(material->type == "PLA");
}

TEST_CASE("Local Connect settings come from the file, the environment wins", "[LocalConnect]")
{
    const auto dir = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path();
    boost::filesystem::create_directories(dir);
    const auto file = dir / "local_connect.json";
    {
        boost::nowide::ofstream f{file.string()};
        f << R"({"url": "http://gx10:8080/", "username": "demo", "password": "secret"})";
    }

    LocalConnectSettings settings = LocalConnectSettings::load(file.string());
    CHECK(settings.enabled());
    CHECK(settings.url == "http://gx10:8080");
    CHECK(settings.username == "demo");
    CHECK(settings.token.empty());

    CHECK_FALSE(LocalConnectSettings::load((dir / "missing.json").string()).enabled());
    boost::filesystem::remove_all(dir);
}
