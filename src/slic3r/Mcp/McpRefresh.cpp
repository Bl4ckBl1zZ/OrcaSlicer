#include "McpRefresh.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/Plater.hpp"

namespace Slic3r { namespace GUI { namespace Mcp {

namespace Refresh {

void object(int obj_idx)
{
    Plater*     plater = wxGetApp().plater();
    ObjectList* list = wxGetApp().obj_list();
    plater->changed_object(obj_idx);
    list->notify_instance_updated(obj_idx);
    list->update_info_items(static_cast<size_t>(obj_idx));
}

void objects(const std::vector<int>& obj_idxs)
{
    for (int obj_idx : obj_idxs)
        object(obj_idx);
}

void full()
{
    Plater* plater = wxGetApp().plater();
    // Force the background-process update, as Plater::priv::update_after_undo_redo does.
    plater->update(false, true);
    wxGetApp().obj_list()->update_after_undo_redo();
}

} // namespace Refresh

}}} // namespace Slic3r::GUI::Mcp
