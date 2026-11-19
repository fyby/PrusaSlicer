#include "Slic3r/Biz/Preset/PrinterPresetSelector.hpp"

#include "Slic3r/Biz/ConnectPrinter/LocalConnect.hpp"
#include "Slic3r/Biz/MatDb/MatDbInteractor.hpp"
#include "Slic3r/Biz/Preset/PresetInteractor.hpp"
#include "Slic3r/Biz/ProjectInteractor.hpp"
#include "Slic3r/Biz/ProjectSettingsInteractor.hpp"
#include "Slic3r/Biz/I18N/I18N.hpp"
#include "Slic3r/Domain/Preset/HwConfig.hpp"

#include "Slic3r/Log.hpp"
#include "nlohmann/json.hpp"
#include "fmt/format.h"

namespace Slic3r::Biz::Preset {

PrinterPresetSelector::PrinterPresetSelector(
    PresetInteractor& preset_interactor,
    ProjectInteractor& project_interactor
) :
    m_preset_interactor(preset_interactor),
    m_project_interactor(project_interactor)
{}

void PrinterPresetSelector::warn(const std::string& msg)
{
    this->invoke_listeners<IPrinterPresetSelectorListener>([&msg](auto* listener) {
        listener->on_printer_preset_selection_warning(msg);
    });
}

void PrinterPresetSelector::select_connect_materials(const nlohmann::json& printer_json)
{
    std::vector<std::string> mismatches;
    select_materials(printer_json, mismatches);
    warn_mismatches(mismatches);
}

void PrinterPresetSelector::warn_mismatches(const std::vector<std::string>& mismatches)
{
    if (mismatches.empty()) {
        return;
    }
    std::string msg;
    for (const std::string& mismatch : mismatches) {
        if (!msg.empty()) {
            msg += '\n';
        }
        msg += mismatch;
    }
    warn(msg);
}

bool PrinterPresetSelector::select_exact_material(size_t slot_idx, const std::string& material_uuid)
{
    if (material_uuid.empty()) {
        return false;
    }
    MatDb::MatDbInteractor& matdb = m_project_interactor.matdb_interactor();
    const std::optional<Domain::MatDb::Material> material = matdb.get_material_by_uuid(material_uuid);
    if (!material.has_value()) {
        SPDLOG_INFO("Connect material sync: material {} is not in the material database", material_uuid);
        return false;
    }
    const MatDb::PresetMatches matches =
        matdb.preset_matches(m_project_interactor.selected_project_id(), slot_idx, *material);
    if (matches.empty()) {
        SPDLOG_INFO("Connect material sync: no preset of slot {} matches {}", slot_idx, material->slug);
        return false;
    }
    SPDLOG_INFO(
        "Connect material sync: slot {} takes {} for {} (rank {})",
        slot_idx,
        matches.front().preset_name,
        material->slug,
        matches.front().rank
    );
    matdb.select_material_preset(slot_idx, *material, matches.front().preset_id);
    return true;
}

void PrinterPresetSelector::select_materials(
    const nlohmann::json& printer_json,
    std::vector<std::string>& mismatches
)
{
    if (!printer_json.contains("tools") || !printer_json["tools"].is_object()) {
        return;
    }

    const auto& tools_json = printer_json["tools"];
    size_t max_slots = m_preset_interactor.material_presets().size();

    const auto& selected_preset = m_preset_interactor.selected_printer_preset();
    const std::string hw_config_id = selected_preset.hw_config.id;
    const std::string printer_preset_id = selected_preset.printer.id;
    const std::string print_preset_id = selected_preset.print.id;

    // The color of the spool in each slot, empty where Connect does not know it.
    std::vector<std::string> colors(max_slots);

    for (size_t slot_idx = 0; slot_idx < max_slots; ++slot_idx) {
        std::string tool_key = std::to_string(slot_idx);

        if (!tools_json.contains(tool_key)) {
            continue;
        }

        const auto& tool_json = tools_json[tool_key];
        const std::optional<ConnectPrinter::LocalToolMaterial> material =
            ConnectPrinter::local_tool_material(tool_json);
        if (!material.has_value()) {
            continue;
        }
        colors[slot_idx] = material->color;

        // A spool of a known material selects the preset made for it.
        if (select_exact_material(slot_idx, material->material_uuid)) {
            continue;
        }

        const std::string target_type = material->type.empty() ? material->abbreviation : material->type;
        if (target_type.empty()) {
            continue;
        }

        std::string current_material_id;
        if (slot_idx < m_preset_interactor.selected_printer_preset().materials.size()) {
            current_material_id = m_preset_interactor.selected_printer_preset().materials[slot_idx].id;
        }

        const auto& material_list       = m_preset_interactor.material_presets().at(slot_idx);
        const auto& available_materials = material_list.items();
        std::string matching_material_id;

        bool current_matches = false;
        std::string best_prusament_id;
        size_t best_prusament_len = std::string::npos;
        std::string best_generic_id;
        size_t best_generic_len = std::string::npos;
        std::string first_match_id;

        for (size_t i = 0; i < available_materials.size(); ++i) {
            const PresetItem& preset = available_materials.at(i);

            for (const auto& [mat_preset, is_runtime] : m_preset_interactor.get_material_presets(hw_config_id, printer_preset_id, print_preset_id, slot_idx)) {
                if (is_runtime) {
                    continue;
                }

                if (mat_preset.get().id != preset.id) {
                    continue;
                }

                auto it = mat_preset.get().config_box().find("filament_type");
                if (it.item && it.item->template get<std::string>() == target_type) {
                    if (preset.id == current_material_id) {
                        current_matches = true;
                    }

                    if (first_match_id.empty()) {
                        first_match_id = preset.id;
                    }

                    if (preset.name.starts_with("Prusament")) {
                        if (preset.name.length() < best_prusament_len) {
                            best_prusament_len = preset.name.length();
                            best_prusament_id = preset.id;
                        }
                    }
                    else if (preset.name.starts_with("Generic")) {
                        if (preset.name.length() < best_generic_len) {
                            best_generic_len = preset.name.length();
                            best_generic_id = preset.id;
                        }
                    }
                }
            }
        }

        if (current_matches) {
            // The preset a person picked for this material type stays.
            continue;
        } else if (!best_prusament_id.empty()) {
            matching_material_id = best_prusament_id;
        } else if (!best_generic_id.empty()) {
            matching_material_id = best_generic_id;
        } else {
            matching_material_id = first_match_id;
        }

        if (!matching_material_id.empty()) {
            m_preset_interactor.select_material_preset(slot_idx, matching_material_id);
        } else {
            SPDLOG_WARN("Connect material sync: No matching material preset found for slot {} (Target type: {})", slot_idx, target_type);
            // TRN Warning line, first {} is the material type, second {} the slot number.
            mismatches.push_back(fmt::format(fmt::runtime(_u8L("No material profile matches {} in slot {}.")), target_type, slot_idx + 1));
        }
    }

    if (std::ranges::any_of(colors, [](const std::string& c) { return !c.empty(); })) {
        m_project_interactor.project_settings_interactor().set_colors_from_connect(
            m_project_interactor.selected_project_id(),
            m_project_interactor.selected_config_container_id(),
            std::move(colors)
        );
    }
}

} // namespace Slic3r::Biz::Preset
