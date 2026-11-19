# MCP server

OrcaSlicer contains a [Model Context Protocol](https://modelcontextprotocol.io) server. When it is
switched on, an AI agent running on the same computer (Claude Code, for example) can inspect the
open project and change it through tools. The agent works in the window the user is looking at:
changes appear live, and every change is one step in the undo history.

The server is **off by default**, listens on this computer only, and requires an access token.
It has no tool that reaches a printer, the cloud account or the network plug-in.

- [Tool reference](#tools)
- [Adding tools](adding-tools.md) (for developers)
- [Error codes](errors.md)

## Switching it on

Preferences > General > **MCP server (AI agent control)**:

| Control | Meaning |
|---|---|
| Enable MCP server | Starts or stops the server at once; no restart needed. |
| Port | TCP port on `127.0.0.1`, default `13720`. If the port is taken the server does not start and the status line says so; it never picks another port, because client configurations hold the address. |
| Status | `Listening on http://127.0.0.1:13720/mcp`, `Stopped`, or the reason it is not running. |
| Connect Claude Code > Copy command | Copies the `claude mcp add ...` command, including the token. |
| Client configuration > Copy JSON | Copies a `.mcp.json` entry, including the token. |
| Access token > Copy token | Copies the token alone. |
| New access token > Regenerate | Replaces the token; clients need the new one. |

The settings are stored in the application configuration as `mcp_enabled` and `mcp_port`. The
token is stored in `<data folder>/mcp/token`. While the server runs,
`<data folder>/mcp/endpoint.json` holds
`{"schema": 1, "url", "port", "token", "pid", "protocolVersions", "app", "appVersion"}` for local
helpers such as the stdio bridge; it is removed when the server stops.

## Connecting a client

Claude Code, from a terminal (the command the Preferences button copies):

```
claude mcp add --transport http orcaslicer http://127.0.0.1:13720/mcp --header "Authorization: Bearer <token>"
```

or as a project file `.mcp.json`:

```json
{
  "mcpServers": {
    "orcaslicer": {
      "type": "http",
      "url": "http://127.0.0.1:13720/mcp",
      "headers": { "Authorization": "Bearer ${ORCA_MCP_TOKEN}" }
    }
  }
}
```

Any MCP client that speaks Streamable HTTP can connect the same way: `POST` to the URL with the
`Authorization: Bearer <token>` header.

## Protocol

One endpoint, `POST /mcp`, answering in both protocol eras:

| Era | Versions | How a client starts |
|---|---|---|
| modern | `2026-07-28` | No handshake. Every request carries its protocol version and client capabilities in `params._meta` and mirrors them in the `MCP-Protocol-Version`, `Mcp-Method` and (for `tools/call`) `Mcp-Name` headers. `server/discover` describes the server. |
| legacy | `2025-11-25`, `2025-06-18`, `2025-03-26` | `initialize`, then `notifications/initialized`. No session id is issued. |

Methods: `server/discover`, `subscriptions/listen` (modern); `initialize`, `ping`,
`notifications/*` (legacy); `tools/list` and `tools/call` (both). The server keeps no
per-connection state in either era; ids, job handles and the scene revision are passed as tool
arguments.

A tool call is answered with one JSON object, or, when the request carries a `progressToken`,
with an event stream of `notifications/progress` followed by the result. Closing the stream (or
`notifications/cancelled` in the legacy era) ends the wait; it does not stop the operation the
tool started.

Tool results are a text block holding one JSON document, plus image blocks for pictures. A
failure the agent can act on is a result with `isError: true` and
`{"error": {"code", "message", "hint", "details"}}`; see [errors.md](errors.md). JSON-RPC errors
are used only for malformed requests, unknown methods and tools, and bad protocol headers.

## Security

| Control | Behaviour |
|---|---|
| Off by default | Nothing listens until the user enables the server. |
| Loopback only | Bound to `127.0.0.1`. |
| Access token | 32 random bytes. Required on every request as `Authorization: Bearer`; compared in constant time; a wrong or missing token gets `401` with no detail. |
| `Origin` | Any request carrying an `Origin` header is rejected with `403`: native clients send none, web pages always do. No CORS headers are ever sent and preflight requests are not answered. |
| `Host` | Must be `127.0.0.1:<port>` or `localhost:<port>`, which defeats DNS rebinding. |
| Method and type | `POST` with `Content-Type: application/json` only. |
| Limits | 8 MB request body, 200 requests per second, 32 connections. |
| Files | Tools read only from the configured read folders (default: Documents, Downloads, Desktop and the workspace) and write only into the write folders (default: the workspace `<data folder>/mcp/workspace`), never over an existing file unless asked to. Configuration keys `mcp_read_roots`, `mcp_write_roots` (`;`-separated). |
| Never exposed | Printer communication, cloud login, the network plug-in, the plug-in system, preferences other than this section. |

The model is: other users of the computer and web pages in the user's browser must not be able to
use the server. Software already running as the user can read the token file and is out of scope.

## Working with the user

Calls run on the application's main thread, one at a time, between the user's own actions.

- A tool that changes something waits while the user is in the middle of something: a modal
  dialog, a mouse drag in the 3D view, an arrange or orient job, a project loading. If that lasts
  longer than the tool's queue time (30 s) the call fails with `ui_busy`. Reading tools still run
  while a dialog is open.
- Dialogs the application raises during a call are answered by the tool's policy and returned to
  the agent as `dialogs`; nothing is left on screen waiting for a click.
- Each changing call is one undo step named `MCP: ...`. The user can undo it like their own, and
  the `history` tool can too. Changes outside the undo stack (presets, filaments) are kept in a
  journal that `history` lists and reverts.
- The agent does not use the user's selection, camera or current tab.

## Conventions

- Millimetres, degrees, seconds, grams. Colours are `#RRGGBB`. Z is up; the bed is the plane z = 0.
- Ids: `o<N>` object, `i<N>` instance, `v<N>` volume (part, modifier, blocker ...), `pl<N>` plate.
  They are stable for the session, survive undo and redo, and change when a project is loaded.
  Plates are also addressed by their 1-based number or `"current"`; filament slots are 1-based.
- Objects and instances are positioned in the frame of their plate (world coordinates minus the
  plate origin). `center` is the centre of the bounding box in XY, `z_min` its lowest point.
- Volumes are positioned in the frame of their object; `center_rel` is 0..1 across the object's
  bounding box.
- `rev` is the scene revision: it changes with every undoable change, whoever made it. Changing
  tools accept `if_rev` and fail with `conflict` if the scene moved on.
- Camera: azimuth 0 looks from the front (from -Y towards +Y) and increases counter-clockwise seen
  from above; elevation 0 is horizontal, 90 looks straight down.

## Tools

One reference page per tool group:

| Group | Tools | Page |
|---|---|---|
| Core | `app_status`, `history`, `job_status`, `job_cancel` | [tools-core.md](tools-core.md) |
| Scene | `scene_get`, `object_update` | [tools-scene.md](tools-scene.md) |
| Settings | `settings_search`, `settings_get`, `preset_list`, `filament_list` | [tools-settings.md](tools-settings.md) |
| View | `screenshot` | [tools-view.md](tools-view.md) |

`tools/list` is the authority on what a build offers.

## Build and environment

The server is built by default; `-DSLIC3R_MCP=OFF` leaves it out together with its hooks.

For automated test launches, environment variables override the preferences (read once at
start-up, nothing is saved):

| Variable | Effect |
|---|---|
| `ORCA_MCP_ENABLE=1` / `0` | Force the server on or off. |
| `ORCA_MCP_PORT=<n>` | Port; `0` picks any free port, reported in `endpoint.json`. |
| `ORCA_MCP_TOKEN=<token>` | Use this token instead of the stored one. |
| `ORCA_MCP_ROOTS=<a;b>` | Extra folders tools may read from and write to. |
| `ORCA_MCP_ENDPOINT_FILE=<path>` | Write a second copy of `endpoint.json` there. |
| `ORCA_MCP_DEBUG_TOOLS=1` | Also register the `debug_*` tools the test-suite uses. |
| `ORCA_MCP_WINDOW=minimized` | Minimise the main window once the application is up (test runs on a desktop in use). |

The client test-suite lives in `tests/mcp`; see [adding-tools.md](adding-tools.md#tests).
