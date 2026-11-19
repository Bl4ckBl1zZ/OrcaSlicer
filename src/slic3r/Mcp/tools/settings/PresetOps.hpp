#ifndef slic3r_Mcp_tools_settings_PresetOps_hpp_
#define slic3r_Mcp_tools_settings_PresetOps_hpp_

// Preset operations shared by the presets, filaments and settings_set tools: resolving a preset by the
// name its selector shows, selecting it through the same path the GUI selector takes, with the GUI's
// "unsaved changes" question turned into an explicit policy, and binding the filament tab to a slot.
//
// Main thread only.

#include <string>
#include <vector>

#include "libslic3r/Preset.hpp"

#include "../../McpJson.hpp"

namespace Slic3r {

class PresetCollection;

namespace GUI {

class Tab;

namespace Mcp { namespace PresetOps {

// "process" | "printer" | "filament" -> Preset::Type; fails with invalid_argument otherwise.
Preset::Type type_from(const std::string& name);
const char*  type_name(Preset::Type type);
PresetCollection& collection(Preset::Type type);
Tab&              tab(Preset::Type type);

// What to do with unsaved edits that a switch would otherwise ask about.
enum class OnUnsaved { Fail, Discard, CarryOver, Save };
OnUnsaved on_unsaved_from(const std::string& name);

struct SelectRequest
{
    Preset::Type type { Preset::TYPE_PRINT };
    int          slot { 0 };          // 1-based filament slot (filament only)
    std::string  name;                // as shown by the selector, or the internal name
    OnUnsaved    on_unsaved { OnUnsaved::Fail };
    std::string  save_as;             // OnUnsaved::Save
};

struct SelectOutcome
{
    std::string before;               // previously selected preset (for the filament: the slot's)
    std::string after;
    bool        changed { false };
    json        discarded = json::object();   // {collection: {key: value}} edits dropped
    json        carried_over = json::array(); // keys re-applied to the new preset
    json        rejected = json::array();     // [{key, reason}] carry-over keys the new preset refused
    json        saved_as = nullptr;           // name of the preset the edits were saved to
    json        notes = json::array();
};

// Resolves a name as its selector shows it (label or alias) or the internal name, to the internal name.
// Fails with unknown_preset (no such preset; nearest names in details) or incompatible (exists but the
// selector does not offer it for the current printer / print).
std::string resolve(Preset::Type type, const std::string& name, int slot);

// Selects a preset as the GUI selector does. Validates and applies the unsaved-changes policy first;
// fails with unsaved_changes (Fail), invalid_argument (bad policy for this switch) before changing
// anything.
SelectOutcome select(const SelectRequest& request);

// {"key": value, ...} of the edited preset's unsaved modifications.
json dirty_values(Preset::Type type);

// Points the filament tab at the preset of `slot` (1-based), as the sidebar's "edit" button does, so
// tab edits land in that preset. Fails with unsaved_changes when the tab holds unsaved edits of a
// different preset. Returns the preset's internal name.
std::string bind_filament_tab(int slot);

// Discards the unsaved edits of the edited preset and reloads its tab.
void discard_edits(Preset::Type type);

}}}} // namespace Slic3r::GUI::Mcp::PresetOps

#endif
