#pragma once

#include <functional>
#include <string>
#include <nlohmann/json.hpp>

#include "../bambu_networking.hpp"
#include "../../GUI/Printer/BambuTunnel.h"

// The runtime sources name the plug-in ABI types through this alias.
namespace BBL = Slic3r;

// BambuSource entry points that the Linux libBambuSource.so exports but the application's
// BambuTunnel.h does not declare. The forwarder and the host pass them through untouched.
#ifndef SLICER_LINUX_RUNTIME_BAMBU_SOURCE_EXTRAS
#define SLICER_LINUX_RUNTIME_BAMBU_SOURCE_EXTRAS
extern "C" {
typedef struct __PlayerEventC
{
    const char* event_name;
    const char* module;
    const char* phase;
    const char* result;
    const char* error_code;
    const char* error_message;
    const char* event_data_body;
} PlayerEventC;

typedef void (*TrackReporter)(void* context, const PlayerEventC* event);

typedef void (*StreamInfoCallback)(void* context, Bambu_StreamInfo* info);

typedef struct __Bambu_SessionStat
{
    long long session_duration_ms;
    long long freeze_total_duration_ms;
    int       freeze_count;
    float     avg_fps;
    float     avg_bitrate_kbps;
    float     avg_jitter_ms;
    float     max_jitter_ms;
} Bambu_SessionStat;
}
#endif

#if defined(SLICER_LINUX_RUNTIME_LIGHTWEIGHT_TASKS) || defined(SLICER_LINUX_RUNTIME_STANDALONE_HOST) || !__has_include("../../../libslic3r/ProjectTask.hpp")
namespace Slic3r {
class BBLModelTask {
public:
    int job_id{0};
    int design_id{0};
    int profile_id{0};
    int instance_id{0};
    std::string task_id;
    std::string model_id;
    std::string model_name;
    std::string profile_name;
    // Appended by the 02.08.02 plug-in ABI. The plug-in fills the object in place, so this
    // mirror must keep the member order and count of libslic3r/ProjectTask.hpp.
    std::string design_title;
    std::string title;
    std::string instance_title;
    std::string create_client;
};
typedef std::function<void(BBLModelTask* subtask)> OnGetSubTaskFn;
}
#else
#include "../../../libslic3r/ProjectTask.hpp"
#endif

namespace Slic3r::SlicerLinuxRuntime {

inline nlohmann::json model_task_to_json(const Slic3r::BBLModelTask* task)
{
    if (!task)
        return nlohmann::json::object();
    return {
        {"job_id", task->job_id},
        {"design_id", task->design_id},
        {"profile_id", task->profile_id},
        {"instance_id", task->instance_id},
        {"task_id", task->task_id},
        {"model_id", task->model_id},
        {"model_name", task->model_name},
        {"profile_name", task->profile_name},
        {"design_title", task->design_title},
        {"title", task->title},
        {"instance_title", task->instance_title},
        {"create_client", task->create_client},
    };
}

inline void json_to_model_task(const nlohmann::json& j, Slic3r::BBLModelTask& task)
{
    task.job_id = j.value("job_id", 0);
    task.design_id = j.value("design_id", 0);
    task.profile_id = j.value("profile_id", 0);
    task.instance_id = j.value("instance_id", 0);
    task.task_id = j.value("task_id", std::string());
    task.model_id = j.value("model_id", std::string());
    task.model_name = j.value("model_name", std::string());
    task.profile_name = j.value("profile_name", std::string());
    task.design_title = j.value("design_title", std::string());
    task.title = j.value("title", std::string());
    task.instance_title = j.value("instance_title", std::string());
    task.create_client = j.value("create_client", std::string());
}

}
