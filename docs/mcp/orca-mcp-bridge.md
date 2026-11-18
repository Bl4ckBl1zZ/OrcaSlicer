# orca-mcp-bridge — connecting stdio MCP clients to OrcaSlicer

OrcaSlicer's MCP server runs inside the running application and listens on
Streamable HTTP at `http://127.0.0.1:<port>/mcp` (port 13720 by default), protected by a
bearer token. Clients that speak HTTP (Claude Code) connect to it directly. Clients that only
launch **stdio** servers (Claude Desktop) launch `orca-mcp-bridge` instead: a small console
program installed next to `orca-slicer`, which forwards newline-delimited JSON-RPC between its
stdin/stdout and the HTTP endpoint.

The bridge finds the server on its own: when the MCP server starts, OrcaSlicer writes an
*endpoint file* with the URL and token, and the bridge reads it for every connection. Nothing
needs to be copied into the client's configuration, and a new token or port is picked up
without restarting the client.

## Claude Desktop

Enable the MCP server in OrcaSlicer's Preferences first. Then open Claude Desktop's
Settings > Developer > Edit Config, which opens `claude_desktop_config.json`:

- Windows: `%APPDATA%\Claude\claude_desktop_config.json`
- macOS: `~/Library/Application Support/Claude/claude_desktop_config.json`
- Linux: `~/.config/Claude/claude_desktop_config.json`

Add an entry under `mcpServers`, with the absolute path of the installed bridge (replace
`<install dir>` with the folder that holds `orca-slicer`). Windows:

```json
{
  "mcpServers": {
    "orcaslicer": {
      "command": "<install dir>\\orca-mcp-bridge.exe",
      "args": []
    }
  }
}
```

For example `"command": "C:\\Program Files\\OrcaSlicer\\orca-mcp-bridge.exe"`.
macOS (the bridge inside the application bundle):

```json
{
  "mcpServers": {
    "orcaslicer": {
      "command": "/Applications/OrcaSlicer.app/Contents/MacOS/orca-mcp-bridge",
      "args": []
    }
  }
}
```

Quit Claude Desktop completely and start it again; it launches the bridge at start-up.

If OrcaSlicer runs with `--datadir` or from a portable folder that the bridge cannot infer, add
`"args": ["--datadir", "<that data directory>"]`. Long tool calls (slicing) are not limited by
the bridge; add `"--request-timeout", "<seconds>"` only if you want a limit.

## Claude Code

Claude Code speaks HTTP, so it connects to the server directly, without the bridge. Copy the
token from OrcaSlicer's Preferences (MCP server) and run:

```bash
claude mcp add --transport http orcaslicer http://127.0.0.1:13720/mcp --header "Authorization: Bearer <token>"
```

To keep the token out of the configuration, put it in an environment variable and use a
project `.mcp.json`:

```json
{
  "mcpServers": {
    "orcaslicer": {
      "type": "http",
      "url": "http://127.0.0.1:13720/mcp",
      "headers": { "Authorization": "Bearer ${ORCA_MCP_TOKEN}" },
      "timeout": 300000
    }
  }
}
```

`timeout` is Claude Code's per-call limit in milliseconds; slicing a large model can take minutes.

## How the bridge finds the server

In order of precedence:

1. `--url` and `--token-file` / `--token` on the command line;
2. the environment: `ORCA_MCP_URL` (or `ORCA_MCP_PORT`, meaning `http://127.0.0.1:<port>/mcp`),
   `ORCA_MCP_TOKEN`, `ORCA_MCP_TOKEN_FILE`;
3. the endpoint file: `--endpoint-file`, `ORCA_MCP_ENDPOINT_FILE`, or
   `<data dir>/mcp/endpoint.json` where the data directory is `--datadir` /
   `ORCA_MCP_DATADIR`, otherwise the one OrcaSlicer itself uses: a `data_dir` folder beside the
   application if it exists (portable install), else the per-user default
   (Windows `%APPDATA%\OrcaSlicer`, macOS `~/Library/Application Support/OrcaSlicer`,
   Linux `$XDG_CONFIG_HOME/OrcaSlicer` or `~/.config/OrcaSlicer`).

A URL given without a token takes the token from the endpoint file when there is one.
Only loopback servers (`127.0.0.0/8`, `::1`, `localhost`) are accepted unless `--allow-remote`
is given. The token is never printed or logged.

### The endpoint file

Written by OrcaSlicer when its MCP server starts listening, rewritten when the port or token
changes, deleted when the server stops or the application exits:

```json
{
  "schema": 1,
  "url": "http://127.0.0.1:13720/mcp",
  "port": 13720,
  "token": "<base64url, 43 characters>",
  "pid": 12345,
  "protocolVersions": ["2026-07-28", "2025-11-25", "2025-06-18", "2025-03-26"],
  "app": "OrcaSlicer",
  "appVersion": "2.5.1"
}
```

- `url` (or, if absent, `port`, meaning `http://127.0.0.1:<port>/mcp`) and `token` are
  required; `pid` lets the bridge tell "the app is not running" from "the server is not
  listening"; the other fields are informational.
- The file is written to a temporary name and renamed into place, so a reader never sees half
  a file, and it is readable by the user only (mode 0600; on Windows an ACL for the user).

The bridge reads it again whenever it changes, when a connection is refused (port changed) and
when the token is rejected (token regenerated), and retries once with the new values.

## Protocol versions

The bridge does not implement MCP; it forwards message bodies unchanged and adds what the
HTTP binding requires. Whatever revision the client speaks is negotiated with the server.

**Current revision (2026-07-28, stateless).** A request is recognised as modern by
`params._meta["io.modelcontextprotocol/protocolVersion"]`. The bridge sends
`MCP-Protocol-Version` (copied from that field), `Mcp-Method`, `Mcp-Name` for `tools/call`,
`prompts/get` and `resources/read` (Base64 sentinel form when not header-safe) and
`Mcp-Param-*` headers for tool arguments annotated with `x-mcp-header` (learned from
`tools/list` results; tools with invalid annotations are removed from the list, as the
specification requires of HTTP clients; a `HeaderMismatch` error on `tools/call` triggers one
`tools/list` refresh and one retry). No session id is sent. A `notifications/cancelled` from
the client closes that request's response stream, which is the cancellation signal on HTTP; the
notification itself is not forwarded. `subscriptions/listen` streams are kept open and their
notifications forwarded.

**Earlier revisions (2025-03-26 to 2025-11-25, `initialize` handshake).** After a successful
`initialize`, the bridge sends the negotiated version in `MCP-Protocol-Version` on every later
request and keeps the `Mcp-Session-Id` if the server issued one. Notifications and the client's
responses to server requests are POSTed (HTTP 202 expected). `notifications/cancelled` is
forwarded and the request's stream is closed. If a session expires (HTTP 404), the bridge
replays the client's `initialize` and `notifications/initialized` once and retries. A response
stream that ends before the response is resumed with `GET` and `Last-Event-ID`. The optional
`GET` stream for server-initiated messages is opened after `notifications/initialized`
(disable with `--no-listen-stream`); a 405 answer is accepted silently. The session is ended
with `DELETE` when the bridge exits.

**When the two sides do not agree** every request still gets an answer: the server's JSON-RPC
error is forwarded with the request's id (a modern server rejecting `initialize` gets a note
that the client must be updated), and an HTTP error without a JSON-RPC body becomes a bridge
error. A client that probes with `server/discover` and gets a non-modern error falls back to
`initialize`, which is what the specification asks of dual-era clients.

Responses and server notifications are written to stdout one per line; SSE comments,
keep-alives, priming events and events of other types are dropped. stdout carries nothing else.

## Errors produced by the bridge

Errors the bridge creates itself carry `data.source = "orca-mcp-bridge"` and a `data.reason`:

| Code | Reason(s) | Meaning |
|---|---|---|
| -31000 | `app_not_running`, `server_not_listening`, `connection_refused`, `connection_lost` | OrcaSlicer is not running, its MCP server is disabled, or the connection broke during a call |
| -31001 | `token_rejected`, `forbidden` | Token does not match (HTTP 401), or Host/Origin refused (403) |
| -31002 | `no_endpoint_file`, `bad_endpoint_file`, `bad_token_file`, `non_loopback`, `bad_url` | No usable configuration |
| -31003 | `timeout`, `busy` | `--request-timeout` elapsed, or the server answered 429 for `--busy-wait` seconds |
| -31004 | `protocol_mismatch`, `http_<status>` | HTTP error without a JSON-RPC body |
| -31005 | `stream_ended`, `empty_response`, `no_response`, `resume_failed` | The server ended the exchange without a response |
| -32700 / -32600 | `parse_error`, `invalid_request`, `message_too_large` | A stdin line that is not a JSON-RPC message (answered with id `null` when no id can be read) |

Exit codes: 0 stdin closed (or SIGINT/SIGTERM), 1 internal error, 2 bad command line, 3 the
client closed stdout, 4 `--check` failed. On stdin end-of-file the bridge closes every open
request stream (cancelling them on the server) and exits at once; `--eof-grace <s>` lets
running requests finish first.

## Troubleshooting

- **Check the connection by hand:** `orca-mcp-bridge --check` (add `--datadir` if needed)
  prints on stderr which endpoint file it read, the URL, whether a token was found and whether
  the server answered `server/discover` or `initialize`.
- **Claude Desktop logs:** `mcp.log` and `mcp-server-orcaslicer.log` (the bridge's stderr) in
  `%APPDATA%\Claude\logs` (Windows), `~/Library/Logs/Claude` (macOS) or
  `~/.config/Claude/logs` (Linux). Add `"-v"` to `args` for debug lines, or
  `"--log-file", "<path>"` to keep a separate log. Logs never contain the token or message
  bodies.
- **"no endpoint file":** OrcaSlicer is not running or its MCP server is disabled. Start the
  app and enable the server in Preferences. If the app uses a non-default data directory, pass
  it with `--datadir`.
- **"left by process N, which has exited":** the app crashed or was killed; start it again.
- **"rejected the access token":** the token was regenerated or the endpoint file belongs to
  another instance. With the endpoint file the bridge re-reads it automatically; with
  `--token`/`--token-file`/`ORCA_MCP_TOKEN`, update the value.
- **"refused the request (HTTP 403)":** the URL must use `127.0.0.1` or `localhost`.
- **Tools missing after an OrcaSlicer restart:** nothing to do in the bridge; it reconnects
  per request. Restart the connector in the client only if the client cached a failure.
- **A long call ends with a timeout:** the bridge has no limit by default; the client's own
  limit applies (Claude Code: `timeout` in `.mcp.json`, `MCP_TOOL_TIMEOUT`).

## Building and testing

The bridge is the `orca-mcp-bridge` target, built with the tree unless `-DORCA_MCP_BRIDGE=OFF`,
and placed next to `orca-slicer` in the build and install trees. It uses header-only Boost
(Asio, Beast, JSON) and nothing else, so it can also be configured on its own:

```bash
cmake -S src/mcp_bridge -B build-bridge -DCMAKE_PREFIX_PATH=<dependency prefix> -DORCA_MCP_BRIDGE_WERROR=ON
cmake --build build-bridge --config Release
```

Tests (Python 3.8+, standard library only) run the real binary over stdio against a mock
dual-era server (`tests/mcp_bridge/mock_server.py`):

```bash
python tests/mcp_bridge/test_bridge.py --bridge build-bridge/orca-mcp-bridge
```

`tests/mcp_bridge/integration_test.py --bridge <bridge> [--datadir <dir>] [--call <tool> '<json>']`
checks a running OrcaSlicer: server conformance over raw HTTP (401, 403, 405, header
validation) and the bridge in both protocol eras.
