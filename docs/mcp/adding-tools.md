# Adding MCP tools

How a tool group is written, registered, tested and documented. The contract is
`src/slic3r/Mcp/McpTool.hpp`; this page explains it and the rules around it.
`src/slic3r/Mcp/tools/ToolsScene.cpp` is the worked example: `scene_get` (read-only) and
`object_update` (mutating, undoable).

## Layout

```
src/slic3r/Mcp/
  Mcp*.hpp / Mcp*.cpp          the core (transport, protocol, registry, dispatch, helpers)
  tools/Tools<Group>.cpp       one tool group per file
  tools/<anything else>        helpers of a group (any name that is not Tools<Group>.cpp)
tests/mcp/
  orca_mcp/                    harness: app launcher, two-era client
  conftest.py                  fixtures (shared instance, `client`, `modern`, `legacy`, `launch_app`)
  test_<group>.py              one test file per group
docs/mcp/
  tools-<group>.md             one reference page per group
```

A group is added by creating `tools/Tools<Group>.cpp`. Nothing else has to be edited: the build
globs the directory and generates the list of groups (`McpSources.cmake`), so groups developed in
parallel do not touch a shared file. After adding or removing a file, an ordinary build re-runs
CMake by itself.

```cpp
#include "../McpTool.hpp"
#include "../McpSchema.hpp"

namespace Slic3r { namespace GUI { namespace Mcp {

namespace {
json plate_validate(ToolContext& ctx) { ... }
} // namespace

MCP_TOOL_GROUP(Slice)   // the name must equal the file name: ToolsSlice.cpp
{
    ToolDef tool;
    tool.name = "plate_validate";
    tool.title = "Validate a plate";
    tool.description = "Check a plate for errors and warnings without slicing. ...";
    tool.input_schema = Schema::parse(R"JSON({
        "type": "object",
        "properties": {"plate": {"$ref": "#/$defs/PlateRef"}},
        "additionalProperties": false})JSON", "plate_validate");
    tool.read_only = true;
    tool.idempotent = true;
    tool.handler = plate_validate;
    registry.add(std::move(tool));
}

}}} // namespace Slic3r::GUI::Mcp
```

Groups register in alphabetical order of their name and tools in the order of the `add()` calls;
`tools/list` reports them in that order, so it is stable between runs. A duplicate tool name, a
schema with an unsupported keyword or an unresolved `$ref` stops the server from starting and is
written to the log: check the log when the endpoint does not come up.

## Describing a tool

| Field | Rule |
|---|---|
| `name` | `snake_case`, grouped by prefix (`object_`, `plate_`, `slice_` ...). |
| `description` | Starts with the words an agent would search for. Says what is returned and when another tool is the better choice. At most 4000 characters; clients load descriptions on demand, so be complete rather than terse. |
| `input_schema` | An object schema with `"additionalProperties": false`, so a misspelt argument is an error. Give every argument a `description` unless its name says it all, and a `default` where there is one. Use the shared argument names ([README](README.md#conventions)): `targets` for several items, `target` for exactly one, `object` for the parent of something new, `action` for a sub-command, `filament` for a 1-based filament slot, `plate` for a plate. Before adding a tool, check whether it is a new `action` or `kind` of an existing one (deleting is `delete`, writing files is `export_file`). |
| `read_only` | The call changes nothing in the application. Clients run read-only calls in parallel and may repeat them. |
| `destructive` | Can discard or overwrite existing work (delete, replace the project, undo). |
| `idempotent` | Repeating the call with the same arguments changes nothing more. Clients may repeat an idempotent call after a dropped connection. |

Schemas use the subset of JSON Schema 2020-12 that `McpSchema.cpp` validates: `type`, `enum`,
`const`, `properties`, `required`, `additionalProperties`, `items`, `minItems`, `maxItems`,
`minimum`, `maximum`, `exclusiveMinimum`, `exclusiveMaximum`, `minLength`, `maxLength`, `pattern`,
`oneOf`, `anyOf`, `allOf`, `not`, `$ref` to `#/$defs/<Name>`. The shared fragments `Id`,
`PlateRef`, `Vec3`, `Vec2`, `Frame`, `Color`, `Scope` and `Camera` are added to a schema
automatically when it references them; a group may define more fragments in its own `$defs`.
Arguments are validated before the handler runs, on the network thread; the handler only sees
arguments that match the schema. What the schema cannot express (ids that must exist, values that
depend on each other) the handler checks and reports with `fail()`.

## The handler

```cpp
json handler(ToolContext& ctx);
```

It runs on the wx main thread, one call at a time, never inside another call and only from the
top-level event loop, so it may use `Model`, `Plater`, presets and GL exactly as GUI code does.
It must not block: no sleeping, no waiting for a background job, no nested event loop of its own.

- **Arguments**: `ctx.args()` is the validated JSON object; `ctx.has("key")`,
  `ctx.arg<T>("key", fallback)`.
- **Ids** (`McpIds.hpp`): `Ids::object(id)`, `Ids::instance(id)`, `Ids::volume(id)`,
  `Ids::item(id)` / `Ids::items(array)` for any kind, `Ids::plate(ref)` for a plate reference
  (number, `pl<N>`, `"current"`), `Ids::filament_slot(n)`. They throw `stale_id` or
  `invalid_argument` with a message that names the argument. Resolve at the start of every call
  and never keep the pointers: undo, redo and project load replace every object.
- **Errors**: `fail(code, message, hint, details)` throws a `ToolError`, which becomes a tool
  result with `isError` and `{"error": {"code", "message", "hint", "details"}}`. The message says
  what is wrong, naming the argument and quoting the value; the hint says what to do next. Reuse
  the codes in [errors.md](errors.md); add a new code only for a new kind of situation, and list
  it there.
- **Result**: return a JSON object. Lists are capped (default 50 items) and say `"truncated":
  true` with a `"next"` cursor when there is more; keep a result under about 20 000 characters.
  The core refuses results over 400 000 characters (`too_large`).
- **Notes**: `ctx.note("...")` for things that happened without being asked for; returned as
  `"notes"`.
- **Images**: `ctx.add_image("image/png", bytes)` adds an image block after the text.

### Mutating tools

A tool with `read_only == false` follows this order, as `object_update` does:

1. Validate everything, resolve every id. Fail before changing anything.
2. `ctx.snapshot("add modifier to o57")` immediately before the first change. This takes one
   undo/redo snapshot named `MCP: add modifier to o57` and suppresses the snapshots that GUI
   functions called afterwards would take: one call is one undo step. Calling it again is a no-op.
3. Change the `Model` (or call the GUI function that does).
4. Refresh (`McpRefresh.hpp`): `Refresh::object(obj_idx)` after changing one object's transform,
   mesh or per-object data; `Refresh::full()` after edits that keep the set of objects and
   instances (volumes, names, flags) and whenever the targeted call is not obviously enough;
   `Refresh::structure()` after adding, removing or reordering objects or instances (it also
   recomputes which plate each instance is on). Call `ctx.set_slice_invalidated()` if a sliced result is no
   longer valid.
5. Return `{"ok": true, "changed": ...}` with the affected subtree in `scene_get` shape, built
   with `Scene::object_json()`, `Scene::volume_json()`, `Scene::plate_json()` or, for tools that
   touch everything, `Scene::summary_json()` (`McpScene.hpp`).

The core adds `"rev"` (the scene revision after the call), `"undo"` (the name of the undo step),
`"journal"`, `"slice_invalidated"`, `"dialogs"` and `"notes"` to the result. Every mutating tool
accepts `if_rev` (added to the schema automatically): the call fails with `conflict` when the
scene revision differs. Set `accepts_if_rev = false` for tools where that makes no sense.

Changes the undo stack does not cover (preset edits and selection, the filament list and colours,
project-level values) are recorded with `ctx.journal(scope, key, before, after, revert)`; the
`history` tool lists them and reverts them by calling `revert`, which must re-apply `before`
through the same code path that set the new value.

Before a mutating tool starts, the dispatcher waits for the UI to be idle: no modal dialog of the
user's, no mouse drag in the 3D view, no running arrange/orient job, no project load. If that does
not happen within `queue_timeout_ms` (30 s by default) the call fails with `ui_busy` and never
reaches the handler. An open gizmo with its own editing state (paint, cut, emboss ...) is closed
and the result notes it. Set `requires_idle_ui = false` only for tools that must run regardless,
such as cancelling a job.

Do not read or change the user's selection, camera, current tab or view options. Where an
existing function can only act on the selection, set the selection, call it and restore the
previous selection; say so in the tool's reference page.

### Modal dialogs

`wxModalDialogHook` intercepts every `ShowModal()`. While a call runs nobody is there to read a
dialog, so the guard answers it and records its title and text; the result returns them as
`"dialogs": [{"title", "message", "answered"}]`.

- A dialog no rule matches is dismissed with its non-destructive button (Cancel, else No, else
  OK/Close) and the call fails with `dialog_blocked`, quoting the dialog. This is the safe default
  for a question the tool did not expect.
- A tool that expects a dialog adds a rule, either statically in `ToolDef::dialog_rules` or per
  call with `ctx.on_dialog({title_contains, text_contains, answer, informational})`, usually
  derived from an argument (`units: "inch"` answers the "convert from inches?" question with Yes).
  Rules added in the handler are tried first; the first match wins.
- `informational = true` (the default for a rule) means the call carries on and the dialog is only
  reported; `false` answers the dialog and then fails the call.
- `ctx.dialogs()` lets the handler look at what was intercepted; `ctx.accept_dialogs()` clears the
  blocked marks when the handler has decided the outcome is fine.

GUI code often defers its dialogs with `CallAfter` (`show_error()` does). The dispatcher therefore
processes the pending events a handler queued before the call ends, still under the call's dialog
rules and undo snapshot. An error box raised that way becomes a `dialog_blocked` failure carrying
its text instead of a window nobody closes.

Outside a call a dialog belongs to the user and is shown normally. While one is open, read-only
tools still run (`app_status` reports it in `busy.modal_dialog`) and mutating tools wait.

### Long-running work

A handler must return quickly. Work that finishes later (a slice, an arrange job, a computation on
a worker thread) uses a job (`McpJobs.hpp`):

```cpp
auto job = Jobs::instance().start("slice", /*cancel*/ [] { ... }, /*dialog rules*/ {});
// wire the application's own progress / completion events:
//   Jobs::instance().update(job, percent, text);
//   Jobs::instance().finish(job, Job::State::Finished, result_json);
ctx.defer(wait_for_job(job, ctx.wait_s(100., 280.)));
return nullptr;                       // ignored after defer()
```

The request stays open for `wait_s` seconds and answers with the finished job
(`{"job", "kind", "status", "percent", "text", "elapsed_s", "result"}`), or with `"status":
"running"` when the time is up. The agent then calls `job_status` with the job id (it long-polls
with its own `wait_s`) and may call `job_cancel`. A waiting call does not hold the queue: other
calls run meanwhile. When the client asked for progress, `notifications/progress` is streamed from
the job's percent and text. A client that stops waiting (closed stream, `notifications/cancelled`)
ends the wait, not the work. Tools that take `wait_s` declare it in their schema
(`{"type": "number", "minimum": 0, "maximum": 280}`); stay under the client's idle limit.

`Job::collect` can build the result lazily when the status is asked for; `dialog_rules` given to
`Jobs::start` answer matching dialogs that open while the job runs (a slicing error at a time
nobody watches) and return them with the job.

For anything else that completes later, `ctx.defer(Pending{poll, on_deadline, on_cancel, wait_s})`
is the primitive `wait_for_job` is built on: `poll` is called about ten times a second from the
top-level event loop.

Two-phase jobs (copy data on the main thread, compute on a worker thread, commit on the main
thread) must not create or modify any `ObjectBase` off the main thread, and must check before
committing that the target still exists and is unchanged, failing with `conflict` otherwise.

### Files

Every path from an agent goes through `Files::check_read(path, extensions)` or
`Files::check_write(path, extensions, overwrite)` (`McpFiles.hpp`) before anything touches the
disk. They return the canonical path or fail with `path_denied`, `not_found` or `exists`.

## Things the server never does

No tool may reach the network agent, cloud login, device manager or the Bambu bridge; send or
upload a print; install, enable or run plugins; or change preferences other than the MCP section.
Settings that run code or hold the printer's address or credentials (`post_process`, `print_host`,
`print_host_webui`, `printhost_*`, `host_type`, `flashforge_serial_number`, plugin-backed options)
are read-only (`Settings::write_refusal`), with secrets redacted (`Settings::value_json_for`, which
every tool returning setting values must use); G-code text settings are writable only
when the user allowed it.

## Tests

One file per group: `tests/mcp/test_<group>.py`. The session fixtures start one private instance
(own data directory, bridge disabled, debug tools on) with `tests/data/20mm_cube.obj` loaded:

- `client` runs the test once per protocol era; `modern` and `legacy` are the two clients.
- `client.tool("name", arg=value)` returns the result document and raises on `isError`;
  `client.tool_error("name", ...)` returns the failure (`.code`, `.message`, `.hint`, `.details`);
  `client.call("name", {...}, progress=True)` returns everything (images, progress, raw result).
- The instance is shared: leave the scene as you found it (`history` undo), or start your own
  with the `launch_app` fixture (`launch_app("mygroup", files=[...])`).
- Set up scenes with the real tools, the way an agent would: `model_import`, `object_transform`,
  `plate_manage`, `settings_get` / `settings_set`, `slice`, `export_file`, and `project_save`
  (`copy: true`) + `project_open` for 3MF round trips. There are no fixture tools. The `debug_*`
  tools (registered with `ORCA_MCP_DEBUG_TOOLS=1`) only inspect or poke the window where no real
  tool can: `debug_echo`, `debug_sleep`, `debug_job`, `debug_dialog`, `debug_ui`, `debug_stats`,
  `debug_touch`, `debug_window`, `debug_viewer_state`, `debug_settings_ui`, `debug_paint_gizmo`,
  `debug_paint_3mf`. Mark such a tool `debug_only = true` and name it `debug_<what>`.
- Test the failures as carefully as the successes: every error code a tool can return, the hint
  text an agent would act on, and for every mutating tool that one undo restores `scene_get`
  exactly.

```
python -m pytest tests/mcp/test_smoke.py        # after every build
python -m pytest tests/mcp/test_<group>.py
python -m pytest tests/mcp                       # everything
python tests/mcp/mcpctl.py start model.stl       # an instance to poke at by hand
python tests/mcp/mcpctl.py call scene_get
python tests/mcp/mcpctl.py stop
```

`ORCA_MCP_APP` names the executable under test, `ORCA_MCP_RUN_DIR` where data directories go.
Several instances can run at once: each takes a free port and its own data directory.

## Documentation

One page per group, `docs/mcp/tools-<group>.md`: for every tool its purpose, arguments, result,
error codes and an example call with its result. Keep it consistent with the registry; the
descriptions in the code are what agents read, the page is what people read.
