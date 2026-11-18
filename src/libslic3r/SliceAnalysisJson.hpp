#ifndef slic3r_SliceAnalysisJson_hpp_
#define slic3r_SliceAnalysisJson_hpp_

// nlohmann::json conversions (serialisation only) for the SliceAnalysis structs:
//   nlohmann::json j = analysis.summary();  j = Slic3r::SliceAnalysis::check_flow(...);
// Box2 serialises as null when empty, else {"min":[x,y],"max":[x,y]}; Box3 adds "min_z"/"max_z" when bounded.

#include "libslic3r/SliceAnalysis.hpp"

#include <nlohmann/json_fwd.hpp>

namespace Slic3r { namespace SliceAnalysis {

void to_json(nlohmann::json &j, const Box2 &v);
void to_json(nlohmann::json &j, const Box3 &v);
void to_json(nlohmann::json &j, const Location &v);
void to_json(nlohmann::json &j, const Verdict &v);
void to_json(nlohmann::json &j, const RoleStats &v);
void to_json(nlohmann::json &j, const FilamentStats &v);
void to_json(nlohmann::json &j, const ObjectStats &v);
void to_json(nlohmann::json &j, const Summary &v);
void to_json(nlohmann::json &j, const LayerRoleStats &v);
void to_json(nlohmann::json &j, const LayerFilamentStats &v);
// The move index range is left out.
void to_json(nlohmann::json &j, const LayerRow &v);
void to_json(nlohmann::json &j, const Cluster &v);
void to_json(nlohmann::json &j, const FlowFilament &v);
void to_json(nlohmann::json &j, const FlowReport &v);
void to_json(nlohmann::json &j, const SpeedRole &v);
void to_json(nlohmann::json &j, const SpeedReport &v);
void to_json(nlohmann::json &j, const CoolingLayer &v);
void to_json(nlohmann::json &j, const CoolingReport &v);
void to_json(nlohmann::json &j, const SupportLayerRow &v);
void to_json(nlohmann::json &j, const SupportObject &v);
void to_json(nlohmann::json &j, const SupportReport &v);
void to_json(nlohmann::json &j, const BoxQuery &v);
void to_json(nlohmann::json &j, const OverhangBand &v);
void to_json(nlohmann::json &j, const OverhangReport &v);
void to_json(nlohmann::json &j, const UnsupportedRegion &v);
void to_json(nlohmann::json &j, const UnsupportedReport &v);
void to_json(nlohmann::json &j, const FirstLayerObject &v);
void to_json(nlohmann::json &j, const FirstLayerReport &v);
void to_json(nlohmann::json &j, const SeamObject &v);
void to_json(nlohmann::json &j, const SeamReport &v);
void to_json(nlohmann::json &j, const ThinFeatureReport &v);
void to_json(nlohmann::json &j, const Transition &v);
void to_json(nlohmann::json &j, const FilamentRange &v);
void to_json(nlohmann::json &j, const MultiMaterialReport &v);
void to_json(nlohmann::json &j, const SectionRole &v);
void to_json(nlohmann::json &j, const SectionFromMoves &v);
void to_json(nlohmann::json &j, const SectionRegion &v);
void to_json(nlohmann::json &j, const SectionFromPrint &v);
void to_json(nlohmann::json &j, const DiffEntry &v);

} } // namespace Slic3r::SliceAnalysis

#endif // slic3r_SliceAnalysisJson_hpp_
