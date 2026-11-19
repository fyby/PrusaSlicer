#pragma once

#include "Slic3r/Biz/Platform/WithListeners.hpp"
#include "Slic3r/Biz/Preset/IPrinterPresetSelectorListener.hpp"

#include <string>
#include <vector>
#include <nlohmann/json_fwd.hpp>

namespace Slic3r::Biz {
class ProjectInteractor;
}

namespace Slic3r::Biz::Preset {

class PresetInteractor;

class PrinterPresetSelector : public WithListeners<IPrinterPresetSelectorListener>
{
public:
    PrinterPresetSelector(PresetInteractor& preset_interactor, ProjectInteractor& project_interactor);

    /**
     * @brief Selects material presets matching the filaments loaded in the Connect printer.
     *
     * A tool whose material carries a material database uuid takes the preset ranked best for
     * that material. Any other tool keeps its preset when the type matches, else takes the
     * Prusament, then the Generic preset of the type. Colors Connect knows replace the slot colors.
     */
    void select_connect_materials(const nlohmann::json& printer_json);

    void warn(const std::string& msg);

private:
    void select_materials(const nlohmann::json& printer_json, std::vector<std::string>& mismatches);

    /// @return false when the material is unknown or no preset of the slot matches it.
    bool select_exact_material(size_t slot_idx, const std::string& material_uuid);

    void warn_mismatches(const std::vector<std::string>& mismatches);

private:
    PresetInteractor& m_preset_interactor;
    /// Owns the material database and the slot colors; used lazily, it is not fully built yet when this is.
    ProjectInteractor& m_project_interactor;
};

} // namespace Slic3r::Biz::Preset
