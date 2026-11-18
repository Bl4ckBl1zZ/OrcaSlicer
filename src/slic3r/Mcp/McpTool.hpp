#ifndef slic3r_Mcp_Tool_hpp_
#define slic3r_Mcp_Tool_hpp_

// The contract between the MCP core and the tool groups: how a tool is described, what its handler
// receives, and how a group registers its tools. See docs/mcp/adding-tools.md.
//
// Threading: handlers, Pending callbacks and every ToolContext member run on the wx main thread,
// one call at a time, never re-entrantly, and only from the application's top-level event loop.
// A handler may therefore touch Model, Plater, presets and GL exactly as GUI code does. It must not
// block: no sleeping, no waiting for a background job, no nested event loop. Work that outlives the
// handler is expressed with ToolContext::defer().

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "McpError.hpp"
#include "McpJson.hpp"

namespace Slic3r { namespace GUI { namespace Mcp {

class Call;
class ToolContext;

// ---------------------------------------------------------------------------------------------
// Modal dialogs
// ---------------------------------------------------------------------------------------------

// What to press on a modal dialog that opens while a tool runs.
enum class DialogAnswer {
    Default, // the non-destructive button: Cancel, else No, else OK/Close
    Ok,
    Cancel,
    Yes,
    No,
    Close
};

// One rule of a tool's dialog policy. A dialog matches when its title contains `title_contains`
// and its text contains `text_contains` (both case-insensitive; an empty string matches anything).
struct DialogRule
{
    std::string  title_contains;
    std::string  text_contains;
    DialogAnswer answer { DialogAnswer::Default };
    // true: the dialog only informs (or the tool chose the answer on purpose); the call carries on
    // and the dialog is reported in "dialogs". false: after the handler returns the call fails
    // with dialog_blocked, quoting the dialog.
    bool         informational { true };
};

// What the guard saw and did; returned to the agent as "dialogs": [{title, message, answered}].
struct DialogRecord
{
    std::string title;
    std::string message;
    std::string kind;     // C++ class of the dialog, for logs and tests
    std::string answered; // "ok", "cancel", "yes", "no", "close"
    bool        blocked { false };
};

json to_json(const DialogRecord& record);

// ---------------------------------------------------------------------------------------------
// Long-running work
// ---------------------------------------------------------------------------------------------

// Returned through ToolContext::defer() by a handler that started something which finishes later
// (a slice, an arrange job, a worker-thread computation). The dispatcher then calls `poll` about
// ten times a second from the top-level event loop until it returns true, the wait runs out, or
// the client goes away. Other calls keep running in between: a deferred call does not hold the
// queue. All three callbacks may throw ToolError.
struct Pending
{
    // Fill `result` and return true when the work is finished.
    std::function<bool(ToolContext&, json& result)> poll;
    // The wait ran out: return the "still running" result (status, percent, job id ...). The work
    // itself keeps going. When empty the call fails with `timeout`.
    std::function<json(ToolContext&)>               on_deadline;
    // The client stopped listening (stream closed or notifications/cancelled). Optional. By design
    // this does not stop the work; stopping is an explicit tool.
    std::function<void(ToolContext&)>               on_cancel;
    // How long to keep the request open, seconds. Tools take it from their `wait_s` argument.
    double                                          wait_s { 100. };
};

// ---------------------------------------------------------------------------------------------
// Per-call context
// ---------------------------------------------------------------------------------------------

struct Image
{
    std::string mime_type; // "image/png" or "image/jpeg"
    std::string bytes;     // raw, not base64
};

class ToolContext
{
public:
    explicit ToolContext(std::shared_ptr<Call> call);
    ~ToolContext();
    ToolContext(const ToolContext&) = delete;
    ToolContext& operator=(const ToolContext&) = delete;

    // The validated arguments (always an object).
    const json& args() const;
    bool        has(const char* key) const;
    // args()[key] converted to T, or `fallback` when the key is absent or null.
    template<class T> T arg(const char* key, T fallback) const
    {
        const json& a = args();
        auto it = a.find(key);
        return (it == a.end() || it->is_null()) ? fallback : it->template get<T>();
    }
    std::string arg(const char* key, const char* fallback) const { return arg<std::string>(key, std::string(fallback)); }

    // --- result extras -------------------------------------------------------------------------
    // A remark for the agent, returned as "notes": [...]. Use it for things that happened but were
    // not asked for ("closed the open Cut gizmo", "slots above 2 were renumbered").
    void note(std::string text);
    // Attach an image block to the result (after the text block).
    void add_image(std::string mime_type, std::string bytes);

    // --- undo ----------------------------------------------------------------------------------
    // Takes ONE undo/redo snapshot named "MCP: <summary>" and suppresses the snapshots that GUI
    // functions called afterwards would take, until the call ends: one tool call = one undo step.
    // Call it after validating the arguments and immediately before the first change. Calling it
    // again in the same call is a no-op. Not for changes outside the undo stack (presets,
    // filament list): those go to the journal (McpJournal.hpp, milestone 3).
    void snapshot(const std::string& summary);
    bool snapshot_taken() const;
    // Records a change the undo stack does not cover (presets, filament list, project values) in
    // the journal (McpJournal.hpp) so `history` can list and revert it. `revert` re-applies
    // `before` through the same code path; pass an empty function when that is not possible.
    // The result gets "journal": [seq, ...]. Returns the entry's sequence number.
    size_t journal(json scope, std::string key, json before, json after, std::function<void()> revert);
    // Marks that the sliced result of some plate was invalidated: adds "slice_invalidated": true.
    void set_slice_invalidated() { m_slice_invalidated = true; }

    // --- modal dialogs -------------------------------------------------------------------------
    // Adds a rule for dialogs that open during this call. Rules added here are tried before the
    // tool's static rules (ToolDef::dialog_rules); the first match wins.
    void on_dialog(DialogRule rule);
    // Dialogs intercepted so far during this call.
    const std::vector<DialogRecord>& dialogs() const;
    // Forget blocked marks: the handler looked at the dialogs and decided the call succeeded.
    void accept_dialogs();

    // --- progress, cancellation, deferral ------------------------------------------------------
    // Progress 0..100 with an optional status line. Sent to the client as notifications/progress
    // when it asked for progress; otherwise dropped. Rate-limited by the transport.
    void progress(double percent, const std::string& text = {});
    // True once the client stopped listening.
    bool cancelled() const;
    // Finish this call later; see Pending. The handler's return value is then ignored.
    void defer(Pending pending);
    // Seconds the client asked to wait: `wait_s` argument clamped to [0, max_s], or default_s.
    double wait_s(double default_s = 100., double max_s = 280.) const;

    // The tool being run.
    const struct ToolDef& tool() const;

private:
    friend class Dispatcher;
    struct SnapshotHolder;

    std::shared_ptr<Call>           m_call;
    std::vector<std::string>        m_notes;
    std::vector<Image>              m_images;
    std::vector<DialogRule>         m_dialog_rules;
    std::vector<DialogRecord>       m_dialogs;
    std::unique_ptr<SnapshotHolder> m_snapshot;
    std::string                     m_snapshot_name;
    std::unique_ptr<Pending>        m_pending;
    std::vector<size_t>             m_journal;
    bool                            m_slice_invalidated { false };
};

// ---------------------------------------------------------------------------------------------
// Tool description
// ---------------------------------------------------------------------------------------------

using ToolHandler = std::function<json(ToolContext&)>;

struct ToolDef
{
    // snake_case, unique. Claude Code shows it as mcp__orcaslicer__<name>.
    std::string name;
    // Short human-readable name.
    std::string title;
    // What the tool does, starting with the words an agent would search for. At most 4000
    // characters; say what is returned and when to prefer another tool.
    std::string description;
    // JSON Schema of the arguments: an object schema. Unknown arguments are rejected when
    // "additionalProperties" is false, which every tool should set. Shared fragments are referenced
    // as {"$ref": "#/$defs/Id"} (see McpSchema.hpp) and are added automatically.
    json        input_schema;

    // MCP annotations.
    bool read_only   { false }; // changes nothing in the app
    bool destructive { false }; // can discard or overwrite existing work
    bool idempotent  { false }; // repeating the call with the same arguments changes nothing more

    // Execution rules.
    // Mutating tools (read_only == false) wait for the UI to be idle before they start: no modal
    // dialog of the user's, no mouse drag, no running arrange/orient job, no project load. An open
    // gizmo is closed. Set this to false for a tool that must run regardless (cancel a job ...).
    bool        requires_idle_ui { true };
    // Accept an optional `if_rev` argument (added to the schema automatically) and fail with
    // `conflict` when the scene revision differs. On by default for mutating tools.
    bool        accepts_if_rev { true };
    // How long a call may sit in the queue waiting for the UI, in ms. 0 = default (15 s for
    // read-only tools, 30 s for the others).
    int         queue_timeout_ms { 0 };
    // Upper bound for the handler itself, in ms; after it the client gets `timeout` even though
    // the main thread is still busy. 0 = default (120 s).
    int         run_timeout_ms { 0 };
    // Static dialog rules, tried after the ones a handler adds with ToolContext::on_dialog().
    std::vector<DialogRule> dialog_rules;
    // Only registered when the server runs with debug tools enabled (ORCA_MCP_DEBUG_TOOLS=1).
    bool        debug_only { false };

    // Runs on the main thread. Returns the result object, or throws ToolError (fail()).
    ToolHandler handler;
};

// ---------------------------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------------------------

class ToolRegistry
{
public:
    // Adds a tool. Throws std::logic_error on a duplicate name, an invalid name or a bad schema,
    // which stops the server from starting: a broken tool must not go unnoticed.
    void add(ToolDef def);

    const ToolDef*              find(const std::string& name) const;
    const std::vector<ToolDef>& tools() const { return m_tools; }
    // The "tools" array of tools/list, built once.
    const json&                 list_json() const { return m_list; }
    // Group each tool was registered by ("Scene", "Settings" ...), for the generated reference.
    const std::string&          group_of(const std::string& tool_name) const;

    // Builds the registry from every tool group compiled into the binary.
    static std::unique_ptr<ToolRegistry> create(bool with_debug_tools);

private:
    void finalize();

    std::vector<ToolDef>               m_tools;
    std::map<std::string, size_t>      m_index;
    std::map<std::string, std::string> m_groups;
    std::string                        m_current_group;
    bool                               m_with_debug_tools { false };
    json                               m_list = json::array();
};

// A tool group is one file src/slic3r/Mcp/tools/Tools<Group>.cpp that defines
//
//     MCP_TOOL_GROUP(<Group>)
//     {
//         registry.add({ ... });
//     }
//
// The build collects the groups from the file names (src/slic3r/Mcp/CMakeLists.txt), so adding a
// group needs no edit to any shared file. Groups are registered in alphabetical order of <Group>,
// tools within a group in the order of the add() calls; tools/list reports them in that order.
#define MCP_TOOL_GROUP(Group) void register_tools_##Group(::Slic3r::GUI::Mcp::ToolRegistry& registry)

}}} // namespace Slic3r::GUI::Mcp

#endif
