#include "EditCommon.hpp"

#include <algorithm>

#include <wx/app.h>
#include <wx/evtloop.h>
#include <wx/timer.h>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Model.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"

#include "../../McpRefresh.hpp"
#include "../../McpScene.hpp"
#include "../../McpUtil.hpp"

namespace Slic3r { namespace GUI { namespace Mcp { namespace Edit {

Plater&        plater() { return *wxGetApp().plater(); }
Model&         model() { return plater().model(); }
PartPlateList& plates() { return plater().get_partplate_list(); }

Vec3d plate_origin(int plate_idx)
{
    PartPlateList& list = plates();
    if (plate_idx < 0 || plate_idx >= list.get_plate_count())
        plate_idx = 0;
    return list.get_plate(plate_idx)->get_origin();
}

Vec3d plate_size()
{
    int    width = 0, depth = 0;
    double height = 0.;
    plates().get_plate_size(width, depth, height);
    return Vec3d(width, depth, height);
}

Vec3d plate_center(int plate_idx)
{
    PartPlateList& list = plates();
    if (plate_idx < 0 || plate_idx >= list.get_plate_count())
        plate_idx = 0;
    // The build volume of the plate (excluding nothing): its XY centre is the bed centre.
    const BoundingBoxf3 box = list.get_plate(plate_idx)->get_build_volume();
    if (box.defined) {
        const Vec3d c = box.center();
        return Vec3d(c.x(), c.y(), 0.);
    }
    const Vec3d size = plate_size();
    return plate_origin(plate_idx) + Vec3d(size.x() / 2., size.y() / 2., 0.);
}

int object_index_by_id(size_t id)
{
    const Model& m = model();
    for (size_t i = 0; i < m.objects.size(); ++i)
        if (m.objects[i]->id().id == id)
            return static_cast<int>(i);
    return -1;
}

std::set<size_t> object_id_set()
{
    std::set<size_t> out;
    for (const ModelObject* object : model().objects)
        out.insert(object->id().id);
    return out;
}

std::vector<int> new_object_indices(const std::set<size_t>& before)
{
    std::vector<int> out;
    const Model&     m = model();
    for (size_t i = 0; i < m.objects.size(); ++i)
        if (before.count(m.objects[i]->id().id) == 0)
            out.push_back(static_cast<int>(i));
    return out;
}

void refresh_structure()
{
    plates().reload_all_objects();
    Refresh::full();
}

json objects_json(const std::vector<int>& obj_idxs)
{
    json out = json::array();
    for (int idx : obj_idxs)
        if (idx >= 0 && idx < static_cast<int>(model().objects.size()))
            out.push_back(Scene::object_json(idx, Scene::Detail::Normal));
    return out;
}

bool main_loop_idle()
{
    if (wxTheApp == nullptr || wxGetApp().is_closing())
        return false;
    wxEventLoopBase* active = wxEventLoopBase::GetActive();
    if (active == nullptr || active != wxTheApp->GetMainLoop() || active->IsYielding())
        return false;
    if (wxGetApp().plater() == nullptr || plater().is_loading_project())
        return false;
    return true;
}

namespace {

// One timer for every watched operation of the group.
class Watcher : public wxTimer
{
public:
    void add(std::function<bool()> poll)
    {
        m_polls.push_back(std::move(poll));
        if (!IsRunning())
            Start(100);
    }

    void Notify() override
    {
        if (!main_loop_idle())
            return;
        // A poll may register another one; iterate over a copy.
        std::vector<std::function<bool()>> polls;
        polls.swap(m_polls);
        std::vector<std::function<bool()>> keep;
        for (auto& poll : polls) {
            bool done = true;
            try {
                done = poll();
            } catch (...) {
                done = true;
            }
            if (!done)
                keep.push_back(std::move(poll));
        }
        for (auto& poll : m_polls)
            keep.push_back(std::move(poll));
        m_polls.swap(keep);
        if (m_polls.empty())
            Stop();
    }

private:
    std::vector<std::function<bool()>> m_polls;
};

Watcher& watcher()
{
    // Never destroyed: a timer must not be destroyed after wxWidgets shut down.
    static Watcher* instance = new Watcher();
    return *instance;
}

} // namespace

void watch(std::function<bool()> poll) { watcher().add(std::move(poll)); }

ScopedAppConfig::~ScopedAppConfig()
{
    AppConfig* config = wxGetApp().app_config;
    if (config == nullptr)
        return;
    for (auto it = m_saved.rbegin(); it != m_saved.rend(); ++it)
        config->set(it->section, it->key, it->value);
}

void ScopedAppConfig::set(const std::string& key, const std::string& value) { set("app", key, value); }

void ScopedAppConfig::set(const std::string& section, const std::string& key, const std::string& value)
{
    AppConfig* config = wxGetApp().app_config;
    m_saved.push_back({section, key, config->get(section, key)});
    config->set(section, key, value);
    m_any = true;
}

void flush_pending_events()
{
    if (wxTheApp != nullptr)
        wxTheApp->ProcessPendingEvents();
}

std::vector<std::string> error_dialog_texts(const ToolContext& ctx)
{
    std::vector<std::string> out;
    for (const DialogRecord& record : ctx.dialogs()) {
        const std::string all = record.title + "\n" + record.message;
        if (contains_ci(all, "error") || contains_ci(all, "failed") || contains_ci(all, "fail ") || contains_ci(all, "invalid") ||
            contains_ci(all, "cannot") || contains_ci(all, "can not") || contains_ci(all, "unable"))
            out.push_back(record.message.empty() ? record.title : record.message);
    }
    return out;
}

std::string path_utf8(const std::string& path) { return path; }

std::string ids_summary(const std::vector<std::string>& ids, size_t max_items)
{
    std::string out;
    for (size_t i = 0; i < ids.size() && i < max_items; ++i)
        out += (i ? " " : "") + ids[i];
    if (ids.size() > max_items)
        out += " ...";
    return out;
}

}}}} // namespace Slic3r::GUI::Mcp::Edit
