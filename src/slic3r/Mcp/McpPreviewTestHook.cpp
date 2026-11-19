// Test hook for McpPreviewRenderer. HOOK-ONLY CODE: the whole file is replaced by the MCP server's
// screenshot tool and can be deleted together with its two call sites (GUI_App.cpp, CMakeLists.txt).
//
// Enabled only when the environment variable ORCA_MCP_PREVIEW_TEST names a job file at start-up.
// The job is a JSON object:
//   {"output_dir": "<dir>", "steps": [ {"op": ...}, ... ]}
// with steps run one after another on the main thread, the event loop running in between:
//   {"op": "load", "files": ["a.stl", "b.3mf"], "arrange": false}   load models (LoadModel only)
//   {"op": "add_filaments", "count": n}                              add n filaments (sidebar "+")
//   {"op": "filament_colors", "colors": ["#RRGGBB", ...]}            set the project's filament colours
//   {"op": "config", "print": {key: "value"}}                        edit the process preset (not saved)
//   {"op": "extruder", "object": i, "extruder": n}                   object i prints with filament n
//   {"op": "move", "object": i, "x": mm, "y": mm, "plate": p}        place instance 0 of object i at (x, y)
//                                                                   of plate p (default: the plate it is on)
//   {"op": "delete_all"}                                             delete every object
//   {"op": "new_plate"}                                              add a plate
//   {"op": "select_plate", "index": p}                               make plate p current
//   {"op": "slice", "scope": "all" | "plate", "wait": true, "stable_polls": 3}   slice like the toolbar
//                                                                   button; wait until every plate is valid
//                                                                   for stable_polls polls 200 ms apart
//   {"op": "invalidate"}                                             mark the results stale (via an edit)
//   {"op": "tab", "name": "prepare" | "preview"}                    switch tab
//   {"op": "window", "state": "minimize" | "restore" | "cover"}
//   {"op": "user_view", "view_type": "speed", "layers": [lo, hi], "camera": "left"}   set the on-screen
//                                                                   preview to a non-default state
//   {"op": "render", "name": "n", "request": {...}, "expect_error": "code"}   render, write n.png, n.json
//   {"op": "repeat", "name": "n", "count": 200, "request": {...}, "sample_every": 10}  leak test
//   {"op": "release"}                                                release the renderer's cache
//   {"op": "wait", "ms": t}
//   {"op": "exit"}                                                   close the application
// A summary of every step is written to <output_dir>/summary.json after each step.

#include "McpPreviewTestHook.hpp"
#include "McpPreviewRenderer.hpp"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/cstdlib.hpp>
#include <boost/nowide/fstream.hpp>
#include <glad/gl.h>
#include <nlohmann/json.hpp>
#include <wx/timer.h>

#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"

#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/GCodeViewer.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GLToolbar.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/IMSlider.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Tab.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

namespace Slic3r { namespace GUI { namespace Mcp {

using json = nlohmann::ordered_json;

namespace {

// Process memory in bytes: private (committed) and resident. -1 where unknown.
std::pair<long long, long long> process_memory()
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX pmc;
    if (::GetProcessMemoryInfo(::GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc)))
        return { (long long)pmc.PrivateUsage, (long long)pmc.WorkingSetSize };
#elif defined(__linux__)
    std::ifstream f("/proc/self/statm");
    long long size = 0, resident = 0;
    if (f >> size >> resident)
        return { size * 4096, resident * 4096 };
#endif
    return { -1, -1 };
}

// Free video memory in KiB from NVX_gpu_memory_info, -1 when the extension is missing.
long long gpu_free_kib()
{
    GLint n = 0;
    ::glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    for (GLint i = 0; i < n; ++i) {
        const char* ext = reinterpret_cast<const char*>(::glGetStringi(GL_EXTENSIONS, GLuint(i)));
        if (ext != nullptr && std::string(ext) == "GL_NVX_gpu_memory_info") {
            GLint kib = 0;
            ::glGetIntegerv(0x9049 /* GL_GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX */, &kib);
            while (::glGetError() != GL_NO_ERROR) {}
            return kib;
        }
    }
    while (::glGetError() != GL_NO_ERROR) {}
    return -1;
}

json matrix_json(const Transform3d& t)
{
    json a = json::array();
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            a.push_back(t.matrix()(r, c));
    return a;
}

// What the user sees: the on-screen camera, tab, plate, and the Preview tab's viewer state.
json user_state()
{
    json s = json::object();
    Plater* plater = wxGetApp().plater();
    MainFrame* frame = wxGetApp().mainframe;
    if (plater == nullptr || frame == nullptr)
        return s;
    const Camera& cam = plater->get_camera();
    s["camera_view"] = matrix_json(cam.get_view_matrix());
    s["camera_projection"] = matrix_json(cam.get_projection_matrix());
    s["camera_zoom"] = cam.get_zoom();
    s["camera_type"] = int(cam.get_type());
    s["camera_viewport"] = json::array({ cam.get_viewport()[0], cam.get_viewport()[1], cam.get_viewport()[2], cam.get_viewport()[3] });
    s["tab"] = frame->m_tabpanel != nullptr ? frame->m_tabpanel->GetSelection() : -1;
    s["preview_shown"] = plater->is_preview_shown();
    s["view3d_shown"] = plater->is_view3D_shown();
    s["current_plate"] = plater->get_partplate_list().get_curr_plate_index();
    s["minimized"] = frame->IsIconized();
    if (GLCanvas3D* preview = plater->get_preview_canvas3D()) {
        GCodeViewer& gv = preview->get_gcode_viewer();
        s["view_type"] = int(gv.get_view_type());
        s["layers_view_range"] = json::array({ gv.get_layers_z_range()[0], gv.get_layers_z_range()[1] });
        s["view_full_range"] = json::array({ gv.get_gcode_view_full_range()[0], gv.get_gcode_view_full_range()[1] });
        s["view_enabled_range"] = json::array({ gv.get_gcode_view_enabled_range()[0], gv.get_gcode_view_enabled_range()[1] });
        s["view_visible_range"] = json::array({ gv.get_gcode_view_visible_range()[0], gv.get_gcode_view_visible_range()[1] });
        s["vertices"] = gv.get_vertices_count();
        s["dim_previous_layers"] = gv.is_dim_previous_layers();
        if (IMSlider* ls = gv.get_layers_slider())
            s["layers_slider"] = json::array({ ls->GetLowerValue(), ls->GetHigherValue(), ls->GetMaxValue() });
        if (IMSlider* ms = gv.get_moves_slider())
            s["moves_slider"] = json::array({ ms->GetLowerValue(), ms->GetHigherValue(), ms->GetMaxValue() });
    }
    return s;
}

class Runner : public wxEvtHandler
{
public:
    Runner(json job, boost::filesystem::path job_dir) : m_job(std::move(job)), m_job_dir(std::move(job_dir))
    {
        m_out = m_job.value("output_dir", (m_job_dir / "out").string());
        boost::filesystem::create_directories(m_out);
        m_steps = m_job.value("steps", json::array());
        m_summary = json::object();
        m_summary["steps"] = json::array();
        m_timer.SetOwner(this);
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) { tick(); });
    }

    void start() { m_timer.StartOnce(m_job.value("start_delay_ms", 1000)); }

private:
    json m_job;
    boost::filesystem::path m_job_dir;
    boost::filesystem::path m_out;
    json m_steps;
    json m_summary;
    size_t m_index{ 0 };
    wxTimer m_timer;
    // a step that waits: polled until it returns true
    std::function<bool()> m_wait;
    std::chrono::steady_clock::time_point m_step_start;
    std::chrono::steady_clock::time_point m_wait_deadline;
    json m_step_result;

    boost::filesystem::path resolve(const std::string& p) const
    {
        boost::filesystem::path path(p);
        return path.is_absolute() ? path : m_job_dir / path;
    }

    void write_json(const boost::filesystem::path& file, const json& j)
    {
        boost::nowide::ofstream f(file.string(), std::ios::binary);
        f << j.dump(2);
    }

    void finish_step(bool ok, const std::string& message = {})
    {
        m_step_result["ok"] = ok;
        if (!message.empty())
            m_step_result["message"] = message;
        m_step_result["ms"] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_step_start).count();
        m_summary["steps"].push_back(m_step_result);
        write_json(m_out / "summary.json", m_summary);
        BOOST_LOG_TRIVIAL(info) << "MCP preview test: step " << m_index << " " << m_step_result.dump();
        ++m_index;
        m_wait = nullptr;
        m_timer.StartOnce(50);
    }

    void tick()
    {
        if (m_wait) {
            bool done = false;
            try {
                done = m_wait();
            } catch (const std::exception& e) {
                finish_step(false, e.what());
                return;
            }
            if (done)
                finish_step(true);
            else if (std::chrono::steady_clock::now() > m_wait_deadline)
                finish_step(false, "timed out");
            else
                m_timer.StartOnce(200);
            return;
        }
        if (m_index >= m_steps.size()) {
            m_summary["finished"] = true;
            write_json(m_out / "summary.json", m_summary);
            return;
        }
        const json& step = m_steps[m_index];
        m_step_start = std::chrono::steady_clock::now();
        m_step_result = json::object();
        m_step_result["index"] = m_index;
        m_step_result["op"] = step.value("op", "");
        if (step.contains("name"))
            m_step_result["name"] = step["name"];
        try {
            run(step);
        } catch (const std::exception& e) {
            finish_step(false, e.what());
        }
    }

    void wait_for(std::function<bool()> cond, double seconds)
    {
        m_wait = std::move(cond);
        m_wait_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(long(seconds * 1000));
        m_timer.StartOnce(200);
    }

    void run(const json& step)
    {
        Plater& plater = *wxGetApp().plater();
        const std::string op = step.value("op", "");
        if (op == "load") {
            std::vector<boost::filesystem::path> files;
            for (const auto& f : step.at("files"))
                files.push_back(resolve(f.get<std::string>()));
            const std::vector<size_t> ids = plater.load_files(files, LoadStrategy::LoadModel, false);
            m_step_result["objects_loaded"] = ids.size();
            if (step.value("arrange", false)) {
                plater.set_prepare_state(Job::PREPARE_STATE_DEFAULT);
                plater.arrange();
                wait_for([&plater] { return plater.get_ui_job_worker().is_idle(); }, 60);
                return;
            }
            finish_step(!ids.empty(), ids.empty() ? "nothing loaded" : "");
        } else if (op == "add_filaments") {
            const int n = step.value("count", 1);
            for (int i = 0; i < n; ++i)
                plater.sidebar().add_filament();
            finish_step(true);
        } else if (op == "filament_colors") {
            DynamicPrintConfig& project = wxGetApp().preset_bundle->project_config;
            auto* colors = project.option<ConfigOptionStrings>("filament_colour");
            const json& list = step.at("colors");
            for (size_t i = 0; i < list.size() && i < colors->values.size(); ++i)
                colors->values[i] = list[i].get<std::string>();
            plater.on_config_change(project);
            plater.sidebar().update_presets(Preset::TYPE_FILAMENT);
            finish_step(true);
        } else if (op == "config") {
            DynamicPrintConfig cfg = wxGetApp().preset_bundle->prints.get_edited_preset().config;
            for (auto it = step.at("print").begin(); it != step.at("print").end(); ++it)
                cfg.set_deserialize_strict(it.key(), it.value().get<std::string>());
            wxGetApp().get_tab(Preset::TYPE_PRINT)->load_config(cfg);
            plater.update();
            finish_step(true);
        } else if (op == "extruder") {
            Model& model = plater.model();
            const size_t obj = step.at("object").get<size_t>();
            if (obj >= model.objects.size())
                throw std::runtime_error("no such object");
            model.objects[obj]->config.set_key_value("extruder", new ConfigOptionInt(step.at("extruder").get<int>()));
            for (ModelVolume* v : model.objects[obj]->volumes)
                v->config.erase("extruder");
            plater.update();
            finish_step(true);
        } else if (op == "move") {
            Model& model = plater.model();
            const size_t obj = step.at("object").get<size_t>();
            if (obj >= model.objects.size())
                throw std::runtime_error("no such object");
            PartPlateList& plates = plater.get_partplate_list();
            int plate_idx = step.value("plate", -1);
            if (plate_idx < 0)
                plate_idx = plates.find_instance(int(obj), 0);
            if (plate_idx < 0)
                plate_idx = 0;
            PartPlate* plate = plates.get_plate(plate_idx);
            if (plate == nullptr)
                throw std::runtime_error("no such plate");
            const Vec3d origin = plate->get_origin();
            ModelInstance* inst = model.objects[obj]->instances[0];
            const Vec3d off = inst->get_offset();
            inst->set_offset(Vec3d(origin.x() + step.at("x").get<double>(), origin.y() + step.at("y").get<double>(), off.z()));
            model.objects[obj]->ensure_on_bed();
            plates.notify_instance_update(int(obj), 0);
            plater.update();
            m_step_result["plate"] = plates.find_instance(int(obj), 0);
            finish_step(true);
        } else if (op == "delete_all") {
            plater.delete_all_objects_from_model();
            finish_step(true);
        } else if (op == "new_plate") {
            m_step_result["plate"] = plater.get_partplate_list().create_plate(true);
            plater.update();
            finish_step(true);
        } else if (op == "select_plate") {
            plater.select_plate(step.at("index").get<int>());
            finish_step(true);
        } else if (op == "slice") {
            const bool all = step.value("scope", "all") == "all";
            plater.update(true, true);
            wxPostEvent(&plater, SimpleEvent(all ? EVT_GLTOOLBAR_SLICE_ALL : EVT_GLTOOLBAR_SLICE_PLATE));
            wxGetApp().mainframe->m_tabpanel->SelectPageByName(TAB_ID_PREVIEW);
            if (!step.value("wait", true)) {
                finish_step(true);
                return;
            }
            auto stable = std::make_shared<int>(0);
            const int polls = step.value("stable_polls", 3);
            wait_for([&plater, all, stable, polls, this] {
                if (plater.is_background_process_slicing()) { *stable = 0; return false; }
                PartPlateList& plates = plater.get_partplate_list();
                bool done = true;
                json valid = json::array();
                for (int i = 0; i < plates.get_plate_count(); ++i) {
                    PartPlate* p = plates.get_plate(i);
                    const bool v = p->is_slice_result_valid();
                    valid.push_back(v);
                    if ((all || i == plates.get_curr_plate_index()) && p->has_printable_instances() && !v)
                        done = false;
                }
                m_step_result["plates_valid"] = valid;
                if (!done) { *stable = 0; return false; }
                return ++*stable >= polls;
            }, step.value("timeout_s", 900.0));
        } else if (op == "invalidate") {
            // the same as a user edit of a print setting that changes the toolpaths
            DynamicPrintConfig cfg = wxGetApp().preset_bundle->prints.get_edited_preset().config;
            const int walls = cfg.opt_int("wall_loops");
            cfg.set_key_value("wall_loops", new ConfigOptionInt(walls == 2 ? 3 : 2));
            wxGetApp().get_tab(Preset::TYPE_PRINT)->load_config(cfg);
            plater.update(true, true);
            m_step_result["wall_loops"] = walls == 2 ? 3 : 2;
            finish_step(true);
        } else if (op == "tab") {
            const std::string name = step.at("name").get<std::string>();
            wxGetApp().mainframe->m_tabpanel->SelectPageByName(name == "preview" ? TAB_ID_PREVIEW : TAB_ID_PREPARE);
            wait_for([] { return true; }, 5);
        } else if (op == "window") {
            const std::string state = step.at("state").get<std::string>();
            MainFrame* frame = wxGetApp().mainframe;
            if (state == "minimize")
                frame->Iconize(true);
            else if (state == "restore") {
                frame->Iconize(false);
                frame->Raise();
            }
            m_step_result["minimized"] = frame->IsIconized();
            wait_for([] { return true; }, 5);
        } else if (op == "user_view") {
            GLCanvas3D* preview = plater.get_preview_canvas3D();
            if (step.contains("camera"))
                plater.get_camera().select_view(step["camera"].get<std::string>());
            if (preview != nullptr) {
                GCodeViewer& gv = preview->get_gcode_viewer();
                if (step.contains("view_type")) {
                    const std::string vt = step["view_type"].get<std::string>();
                    libvgcode::EViewType type = libvgcode::EViewType::Speed;
                    if (GCodeViewer::view_type_from_config_name(vt, type))
                        gv.select_view_type(type);
                    m_step_result["view_type"] = int(gv.get_view_type());
                }
                if (step.contains("layers") && gv.get_layers_count() > 0) {
                    IMSlider* ls = gv.get_layers_slider();
                    const int lo = step["layers"][0].get<int>(), hi = step["layers"][1].get<int>();
                    ls->SetLowerValue(lo);
                    ls->SetHigherValue(hi);
                    gv.set_layers_z_range({ unsigned(lo), unsigned(hi) });
                }
                preview->set_as_dirty();
                preview->request_extra_frame();
            }
            plater.get_current_canvas3D()->set_as_dirty();
            m_step_result["state"] = user_state();
            wait_for([] { return true; }, 5);
        } else if (op == "render") {
            do_render(step);
        } else if (op == "repeat") {
            do_repeat(step);
        } else if (op == "release") {
            PreviewRenderer::instance().release();
            finish_step(true);
        } else if (op == "wait") {
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(step.value("ms", 500));
            wait_for([until] { return std::chrono::steady_clock::now() >= until; }, 3600);
        } else if (op == "exit") {
            m_summary["finished"] = true;
            write_json(m_out / "summary.json", m_summary);
            PreviewRenderer::instance().shutdown();
            wxGetApp().mainframe->Close(true);
        } else
            finish_step(false, "unknown op \"" + op + "\"");
    }

    void do_render(const json& step)
    {
        const std::string name = step.value("name", "render" + std::to_string(m_index));
        PreviewRequest req;
        std::string error;
        if (!preview_request_from_json(step.value("request", json::object()), req, error)) {
            json j = json::object();
            j["ok"] = false;
            j["error"] = json::object({ { "code", "invalid_argument" }, { "message", error } });
            write_json(m_out / (name + ".json"), j);
            const bool expected = step.value("expect_error", "") == "invalid_argument";
            m_step_result["error"] = error;
            finish_step(expected, expected ? "" : error);
            return;
        }
        const json before = user_state();
        const auto mem_before = process_memory();
        PreviewImage img = PreviewRenderer::instance().render(req);
        const auto mem_after = process_memory();
        const json after = user_state();

        json j = to_json(img);
        j["user_state_before"] = before;
        j["user_state_after"] = after;
        j["user_state_unchanged"] = before == after;
        j["process_private_bytes_before"] = mem_before.first;
        j["process_private_bytes_after"] = mem_after.first;
        write_json(m_out / (name + ".json"), j);
        if (img.ok && !img.png.empty()) {
            boost::nowide::ofstream f((m_out / (name + ".png")).string(), std::ios::binary);
            f.write(img.png.data(), std::streamsize(img.png.size()));
        }
        m_step_result["user_state_unchanged"] = before == after;
        const std::string expect = step.value("expect_error", "");
        if (!img.ok) {
            m_step_result["error_code"] = img.error_code;
            finish_step(expect == img.error_code, expect == img.error_code ? "" : img.error_message);
        } else {
            m_step_result["total_ms"] = img.info["perf"]["total_ms"];
            m_step_result["cache_hit"] = img.info["perf"]["cache_hit"];
            finish_step(expect.empty() && before == after, before == after ? "" : "user state changed");
        }
    }

    void do_repeat(const json& step)
    {
        const std::string name = step.value("name", "repeat" + std::to_string(m_index));
        PreviewRequest req;
        std::string error;
        if (!preview_request_from_json(step.value("request", json::object()), req, error)) {
            finish_step(false, error);
            return;
        }
        const int count = step.value("count", 200);
        const int every = std::max(1, step.value("sample_every", 10));
        const bool vary = step.value("vary_camera", true);
        json samples = json::array();
        bool all_ok = true;
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < count; ++i) {
            if (vary) {
                req.camera.view.clear();
                req.camera.azimuth_deg = double(i * 7 % 360);
                req.camera.elevation_deg = double(10 + i * 3 % 70);
            }
            PreviewImage img = PreviewRenderer::instance().render(req);
            all_ok = all_ok && img.ok;
            if (i % every == 0 || i == count - 1) {
                const auto mem = process_memory();
                json s = json::object();
                s["i"] = i;
                s["ok"] = img.ok;
                s["private_bytes"] = mem.first;
                s["working_set_bytes"] = mem.second;
                s["gpu_free_kib"] = gpu_free_kib();
                s["viewer_gpu_bytes"] = PreviewRenderer::instance().stats().gpu_bytes;
                s["total_ms"] = img.ok ? img.info["perf"]["total_ms"] : json(nullptr);
                samples.push_back(s);
            }
            // let the event loop run (the user's canvas repaints between renders)
            wxGetApp().Yield(true);
        }
        json j = json::object();
        j["count"] = count;
        j["seconds"] = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        j["samples"] = samples;
        write_json(m_out / (name + ".json"), j);
        m_step_result["first"] = samples.front();
        m_step_result["last"] = samples.back();
        finish_step(all_ok);
    }
};

} // namespace

void PreviewTestHook::start_if_requested()
{
    const char* env = boost::nowide::getenv("ORCA_MCP_PREVIEW_TEST");
    if (env == nullptr || *env == 0)
        return;
    const boost::filesystem::path job_file(env);
    json job;
    try {
        boost::nowide::ifstream f(job_file.string());
        job = json::parse(f);
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "MCP preview test: cannot read " << job_file.string() << ": " << e.what();
        return;
    }
    BOOST_LOG_TRIVIAL(info) << "MCP preview test: running " << job_file.string();
    // leaked on purpose: it lives until the application exits
    Runner* runner = new Runner(std::move(job), job_file.parent_path());
    runner->start();
}

}}} // namespace Slic3r::GUI::Mcp
