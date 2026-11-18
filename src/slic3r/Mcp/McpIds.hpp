#ifndef slic3r_Mcp_Ids_hpp_
#define slic3r_Mcp_Ids_hpp_

// Stable ids for the things tools address, and their resolution to live pointers.
//
//   o<N>  ModelObject      i<N>  ModelInstance      v<N>  ModelVolume      pl<N>  PartPlate
//
// N is ObjectBase::id(), unique across all types within a session. Ids survive reordering,
// deletion of other items and undo/redo; a project load assigns new ones. Plates are also addressed
// by their 1-based number or "current"; filament slots by their 1-based number.
//
// Resolve at the start of every call and never keep the pointers: undo/redo and project load
// replace every object. A missing id fails with `stale_id`, a wrong kind with `invalid_argument`
// ("v59 is a volume; this argument needs an object").
//
// Main thread only.

#include <string>
#include <vector>

#include "McpJson.hpp"

namespace Slic3r {

class Model;
class ModelObject;
class ModelInstance;
class ModelVolume;

namespace GUI {

class PartPlate;

namespace Mcp {

enum class IdKind { Object, Instance, Volume, Plate };

struct ObjectRef
{
    ModelObject* object { nullptr };
    int          obj_idx { -1 };
};

struct InstanceRef
{
    ModelObject*   object { nullptr };
    int            obj_idx { -1 };
    ModelInstance* instance { nullptr };
    int            inst_idx { -1 };
};

struct VolumeRef
{
    ModelObject* object { nullptr };
    int          obj_idx { -1 };
    ModelVolume* volume { nullptr };
    int          vol_idx { -1 };
};

// Result of resolving an id whose kind the caller does not know in advance.
struct ItemRef
{
    IdKind         kind { IdKind::Object };
    ModelObject*   object { nullptr }; // the object itself, or the owner of the instance / volume
    int            obj_idx { -1 };
    ModelInstance* instance { nullptr };
    int            inst_idx { -1 };
    ModelVolume*   volume { nullptr };
    int            vol_idx { -1 };
    int            plate_idx { -1 }; // IdKind::Plate
};

namespace Ids {

std::string of(const ModelObject& object);
std::string of(const ModelInstance& instance);
std::string of(const ModelVolume& volume);
std::string of(PartPlate& plate);

// `what` names the argument in error messages ("target", "objects[2]").
ObjectRef   object(const std::string& id, const char* what = "id");
InstanceRef instance(const std::string& id, const char* what = "id");
VolumeRef   volume(const std::string& id, const char* what = "id");
// Any o / i / v id (and pl ids when `allow_plate`).
ItemRef     item(const std::string& id, const char* what = "id", bool allow_plate = false);
// A JSON array of ids -> items; fails on the first bad one.
std::vector<ItemRef> items(const json& ids, const char* what = "targets", bool allow_plate = false);

// A PlateRef argument (1-based number, "pl<N>" or "current") -> 0-based plate index.
// A null / missing value means the current plate.
int plate(const json& ref, const char* what = "plate");
// 0-based index of the plate the instance sits on, or -1 when it is on none.
int plate_of_instance(int obj_idx, int inst_idx);

// A 1-based filament slot argument -> 0-based index, range-checked against the project's filaments.
int filament_slot(const json& slot, const char* what = "filament");
int filament_count();

} // namespace Ids

}}} // namespace Slic3r::GUI::Mcp

#endif
