# Tools: Printers

`printer_list`, `printer_status`, `print_send`, `printer_control`. Source:
`src/slic3r/Mcp/tools/ToolsPrinters.cpp`, helpers in `src/slic3r/Mcp/tools/printers/`
(`PrinterAccess.cpp`: switches, ids, journal; `PrinterRegistry.cpp`: the printers and their
status; `HostClient.cpp`: OctoPrint and Moonraker over HTTP; `PrintSend.cpp`: sending and control;
`SendConfirm.cpp`: the confirmation window). Tests: `tests/mcp/test_printers.py`. Error codes:
[errors.md](errors.md#printers).

These are the only tools that reach a printer. They use the printers the application already
knows, through the application's own code (the device manager, the Print plate job, the print host
upload). There is still no tool for cloud login, the account or the network plug-in.

## Access: switches, off by default

Preferences > General > **MCP server (AI agent control)**:

| Switch | Configuration key | Allows |
|---|---|---|
| Allow AI agents to read printer status | `mcp_printer_read` | `printer_list`, `printer_status` |
| Allow AI agents to send print jobs | `mcp_printer_send` | `print_send`, `printer_control` |
| Send print jobs without asking me | `mcp_printer_send_unattended` | sends and stops without the confirmation window (only while `mcp_printer_send` is on) |

- All are off by default. The first two are independent of each other: a user can allow sending without
  allowing listing, in which case the agent addresses a printer by its exact name.
- While a switch is off, its tools fail with `printer_access_disabled`; the hint names the switch.
  The refusal is written to the journal (`refused`).
- No tool can change them. Only the Preferences dialog writes them; no tool argument names them,
  and the code that lets a tool change an application setting for the length of one call refuses
  every `mcp_*` key. An agent that needs access asks the user.
- With sending allowed, every send and every stop waits for the user's click in a
  [confirmation window](#the-confirmation-window). Pause and resume do not.
- With **Send print jobs without asking me** also on, no window opens: a request that passes the
  checks is taken as confirmed and transferred, so the user can have the agent start prints when
  they are away from the computer (from a phone, for example). Every check, the queue, the temp file
  and the journal stay as they are; the journal marks these answers `unattended`. Nothing checks
  the printer physically (a clear bed, a removed part), so the switch is the user's decision to
  trust the agent with that.
- `app_status` reports the switches: `"printers": {"read_allowed", "send_allowed",
  "send_unattended", "sends_in_flight"}` (requests waiting for the user or being transferred).

## What is never returned

Returned: printer names (the Bambu Lab device name, or the printer preset's name), model, kind,
connection type, state, job progress, temperatures, nozzle, filament trays, error codes and
messages, compatibility with the selected preset.

Never returned: access codes, serial numbers (device ids), IP addresses and host names, API keys,
passwords, or anything of the user's cloud account (login, e-mail, tokens). Host errors are
reworded so they name neither the host nor its address, and upload errors have the host replaced
by `<printer host>`.

## Printers and ids

| Kind | Listed when | Status | `print_send` | `printer_control` |
|---|---|---|---|---|
| Bambu Lab: `kind: "bambu"`, `connection` `lan` or `cloud` | found on the LAN or bound to the user's cloud account (a printer in both is listed once) | live only from the printer selected in the Device tab | the application's own print job | pause, resume, stop |
| Host printer: `kind` is the host type (`octoprint`, `moonraker`, `prusalink` ...), `connection: "host"` | an FFF printer preset has a print host (`print_host`) | OctoPrint and Moonraker over HTTP; other types report `state: "unknown"`, `status_supported: false` | upload through the application's print host code | OctoPrint and Moonraker only |

Order: Bambu Lab printers by name, then host printers by name.

Ids are opaque: `prn_<10 hex>` for a Bambu Lab printer, `host_<10 hex>` for a host printer, a
salted hash of the serial number or the preset name. The salt lives in
`<data folder>/mcp/printer_salt` and never leaves the application. Ids are stable across sessions of
one data folder; renaming a host printer's preset gives it a new id. (If the salt cannot be
written, ids change at every start; the log says so.)

Every tool takes `printer`: an id, or a printer's exact name (case does not matter). Several
printers with that name fail with `ambiguous_printer` (`details.ids`); no match fails with
`printer_not_found` (`details.printers` lists `{id, name}` of all known printers).

### Status fields

| Field | |
|---|---|
| `online` | `true`, `false`, or `null` when not known. |
| `state` | `idle`, `preparing`, `printing`, `paused`, `finished`, `failed`, `error` (hosts), `offline`, `unknown` (no status yet, or a host type the server cannot read). |
| `live` | Bambu Lab: the application is connected and the last report is under 15 s old. |
| `status_age_s` | Bambu Lab: seconds since the last report; `null` = none yet (then a `note`, and nothing else below). |
| `job` | `{name, percent, layer: [current, total], remaining_s}`, the fields the printer reports, while preparing, printing or paused (`printer_status` also when finished or failed). |
| `temperatures` | `{nozzle: {current, target}, bed: {current, target}}`. `printer_status` only. |
| `nozzle` | Bambu Lab: `{diameter_mm, type}` (a list for several nozzles). |
| `trays` | Bambu Lab: `[{tray, empty, type, color, remain_percent, filament_id}]`; see [tray labels](#tray-labels). |
| `errors` | Bambu Lab: `{code: "HMS_...", level}` per HMS item and `{code, level: "print_error"}`; `printer_status` adds `message`. Hosts: `{code: "host_error", message}` in state `error`. |
| `error` | Hosts: why the host gave no status (`"the printer host did not answer"`, `"the printer host refused the API key (HTTP 401)"`, `"OctoPrint is not connected to the printer"` ...). |
| `compatibility` | `{compatible, selected_preset, reasons?, warnings?}`, see below. |

Bambu Lab states come from the printer's own status (`RUNNING` printing, `PAUSE` paused,
`PREPARE` / `SLICING` / `INIT` preparing, `FINISH` finished, `FAILED` failed, anything else idle).
OctoPrint: the `error` / `closedOrError` flags give `error`, `paused` / `pausing` give `paused`,
`printing` / `cancelling` give `printing`; HTTP 409 (OctoPrint runs but is not connected to the
printer) gives `online: true, state: "offline"`. Moonraker: `print_stats.state` `printing`,
`paused`, `complete` (finished), `error`.

`compatibility` compares the printer with the selected printer preset (the one the plate is sliced
for, with its unsaved edits):

- Bambu Lab: the selected preset must be a Bambu Lab printer and its model compatible with the
  printer's (a compatible but different model is a `warnings` entry), and the nozzle diameter must
  match within 0.01 mm. Values the printer does not report do not make it incompatible.
- Host printer: compatible when its preset is the selected one; otherwise both presets must name
  the same, non-empty `printer_model` and the nozzle diameters must match within 0.01 mm.

## printer_list

Lists every printer the application knows. Read-only. Needs `mcp_printer_read`.

| Argument | Type | Default | |
|---|---|---|---|
| `probe_hosts` | boolean | `true` | Ask OctoPrint and Moonraker hosts for their state (waits up to 8 s). `false`: their last known state. |

Bambu Lab printers are never contacted by this tool: they show what the device manager last
received, so a printer that is not selected in the Device tab has an older status
(`status_age_s`, `live: false`). Call `printer_status` for a live one. A host that has not answered
within 8 s is listed with `online: null, state: "unknown", error: "no answer yet"`. Entries carry no
`temperatures`; host entries carry no `job`; Bambu Lab `errors` carry no `message`.

```json
{"printers": [
   {"id": "prn_3fa9c01b2e", "name": "A1 Studio", "kind": "bambu", "connection": "lan", "model": "Bambu Lab A1",
    "online": true, "state": "idle", "live": false, "status_age_s": 42,
    "nozzle": {"diameter_mm": 0.4, "type": "stainless_steel"},
    "trays": [{"tray": "A1", "empty": false, "type": "PLA", "color": "#FFFFFF", "remain_percent": 80, "filament_id": "GFA00"},
              {"tray": "A3", "empty": true}, {"tray": "ext", "empty": true}],
    "errors": [],
    "compatibility": {"compatible": true, "selected_preset": "Bambu Lab A1 0.4 nozzle"}},
   {"id": "host_8c2d71e04a", "name": "A1 Octo", "kind": "octoprint", "connection": "host", "model": "Bambu Lab A1",
    "online": true, "state": "idle", "errors": [],
    "compatibility": {"compatible": true, "selected_preset": "Bambu Lab A1 0.4 nozzle"}},
   {"id": "prn_c41e90d7b2", "name": "P1S Office", "kind": "bambu", "connection": "lan", "model": "Bambu Lab P1S",
    "online": true, "state": "idle", "live": false, "status_age_s": 310, "trays": [], "errors": [],
    "compatibility": {"compatible": false, "selected_preset": "Bambu Lab A1 0.4 nozzle",
                      "reasons": ["the plate is sliced for Bambu Lab A1, the printer is a Bambu Lab P1S"]}}],
 "selected_printer_preset": "Bambu Lab A1 0.4 nozzle"}
```

With no printer: `"printers": []` and a `note` saying why (no Bambu Lab printer discovered or bound
to the account, no printer preset with a print host).

Journal: `list` with `{count}`.

## printer_status

Live status of one printer. Read-only. Needs `mcp_printer_read`.

| Argument | Type | Default | |
|---|---|---|---|
| `printer` | string | required | Id or exact name. |
| `wait_s` | number 0..30 | 10 | Seconds to wait for a live status. |

- **Bambu Lab.** A Bambu Lab printer reports status only while it is the printer selected in the
  Device tab. If the printer is offline or already live, the tool answers at once. Otherwise it
  selects the printer, as a click in the Device tab would, which disconnects the previously
  selected LAN printer (`note` says so; journal `status_connect`), and waits up to `wait_s` for the
  first report. If none arrives, the result is the last status received, `live: false` and a note:
  call again to wait longer. `printer_not_found` if the application forgot the printer meanwhile.
- **OctoPrint, Moonraker.** Asked over HTTP (OctoPrint: `/api/printer`, then `/api/job`;
  Moonraker: one `printer/objects/query`), with the preset's API key. The tool waits at least 8 s;
  a host that has not answered by then gives `online: null, state: "unknown"` and an `error`.
- **Other host types.** `online: null, state: "unknown", status_supported: false` and a note.

```json
{"id": "prn_51b0e7a9c3", "name": "A1 Garage", "kind": "bambu", "connection": "lan", "model": "Bambu Lab A1",
 "online": true, "state": "printing", "live": true, "status_age_s": 1,
 "nozzle": {"diameter_mm": 0.4, "type": "stainless_steel"},
 "trays": [{"tray": "A1", "empty": false, "type": "PLA", "color": "#FFFFFF", "remain_percent": 80}],
 "job": {"name": "Busy job", "percent": 40, "layer": [20, 50], "remaining_s": 1800},
 "temperatures": {"nozzle": {"current": 219, "target": 220}, "bed": {"current": 64.0, "target": 65.0}},
 "errors": [{"code": "HMS_0500020000020001", "level": 2, "message": "..."}],
 "compatibility": {"compatible": true, "selected_preset": "Bambu Lab A1 0.4 nozzle"}}
```

Journal: `status`, and `status_connect` when the tool selected the printer.

## print_send

Sends a sliced plate to a printer after the user confirms it. Needs `mcp_printer_send`. Not
read-only, not idempotent; takes no `if_rev`.

| Argument | Type | Default | |
|---|---|---|---|
| `plate` | PlateRef | current plate | |
| `printer` | string | required | Id or exact name. |
| `ams_mapping` | `"auto"` \| `[{filament, tray}]` (1..32 entries) | `"auto"` | Bambu Lab only: which tray feeds each filament; see [AMS mapping](#ams-mapping). A list for a host printer is `invalid_argument`. |
| `options.bed_leveling` | `auto` \| `on` \| `off` | `auto` | Bambu Lab. |
| `options.flow_calibration` | `auto` \| `on` \| `off` | `auto` | Bambu Lab. Ignored (with a warning when given) if the printer has none. |
| `options.timelapse` | boolean | `false` | Bambu Lab. Switched off (with a warning) if the printer cannot record one. |
| `options.use_ams` | boolean | `true` | Bambu Lab. `false`: print from the external spool. |
| `options.storage` | `auto` \| `sdcard` \| `internal` | `auto` | Bambu Lab. |
| `options.start_print` | boolean | `true` | Host printers: start printing after the upload. |
| `options.file_name` | string, up to 120 | `<project>_plate_<n>` | Host printers: file name on the host. No folders or `/ \ : * ? " < > \|`. The extension (`.gcode`, or `.gcode.3mf`) is added when missing; the default name is the project name folded to ASCII. |
| `wait_s` | number 0..280 | 10 | Seconds to wait for the outcome before returning the running job. |

Options of the other printer kind are ignored.

### Checks

The call checks, in this order, and fails without asking the user and without sending anything:

| Check | Code |
|---|---|
| Switch on | `printer_access_disabled` |
| `plate`, `printer` arguments | `invalid_argument`, `stale_id`, `printer_not_found`, `ambiguous_printer` |
| No unsaved post-processing scripts or slicing plug-ins in the process preset | `untrusted_scripts` |
| The plate is sliced and unchanged since, with its G-code on disk | `busy_slicing`, `slice_stale`, `not_sliced` |
| The printer matches the preset the plate was sliced for | `printer_incompatible` (`details` is the compatibility) |
| Room in the queue | `rate_limited` |
| Bambu Lab: online, live, not busy (idle, finished or failed) | `printer_offline`, `printer_status_unknown`, `printer_busy` (`details` is the status) |
| Bambu Lab: one nozzle | `unsupported_printer` (two-nozzle printers: the user sends from the window) |
| Bambu Lab: storage | `no_storage` (`sdcard` without a usable card, `internal` without internal storage, or a LAN printer with neither) |
| Bambu Lab: the plate uses a filament | `not_sliced` |
| Bambu Lab: filament mapping | `invalid_argument`, `incomplete_mapping`, `tray_not_found`, `no_matching_tray`, `tray_empty`, `filament_type_mismatch` |
| Host: the printer preset still exists | `printer_not_found` |
| Host: no `ams_mapping` list, a plain `file_name` | `invalid_argument` |
| Host: the process preset has no post-processing scripts (the server does not run them, so the file would differ from what the window's Send button uploads) | `post_processing_not_supported` |
| Writing the file to send | `send_failed` |
| Host: the host answers (the call waits up to 15 s, or `wait_s` if longer) and is not printing, paused or in error | `printer_offline`, `printer_busy` |
| Host: room in the queue (again) | `rate_limited` |

From `untrusted_scripts` on, each refusal is written to the journal as `send_refused` with its
`reason`, except `rate_limited` and a host that does not answer in time.

For a host type the server cannot read status from (anything but OctoPrint and Moonraker) the
online and busy checks cannot be made: the request goes to the confirmation window with the warning
"OrcaSlicer cannot read the state of this printer host; make sure it is idle and ready before
sending" (also in the result's `warnings`), and the user decides.

Confirmed Bambu Lab jobs are transferred one after the other: a second confirmed send waits for
the first transfer to finish instead of cancelling it.

### What is sent

When the checks pass, the file to send is written at once to a private temporary file under
`<data folder>/mcp/send/`. What the user confirms is what is sent, whatever happens to the scene
afterwards. The file is deleted when the request ends.

- Bambu Lab: the plate as a `.gcode.3mf` (for a cloud printer also its configuration 3MF), sent by
  the application's own print job (the one behind Print plate), with the tray mapping and options.
  The window does not switch to the Device tab.
- Host printer: a copy of the plate's sliced G-code, or a `.gcode.3mf` when the host printer's
  preset has "Use 3MF instead of G-code" (`use_3mf`; the upload then names the plate). Uploaded with
  the application's print host code, which asks the host to start printing unless
  `options.start_print` is `false`.

Then the request joins the [queue](#queue-and-rate-limit) and the call returns within `wait_s`.
After the user clicks Send, the printer is checked once more (Bambu Lab: still online and not
busy; host: its last answer was online, without asking it again); otherwise nothing is sent and the
outcome is `not_sent`. The same check runs when a queued request's turn comes.

### Result

A job of kind `print_send` (see `job_status`). While it waits:

```json
{"job": "job12", "kind": "print_send", "status": "running", "percent": 0,
 "text": "waiting for the user to confirm in the OrcaSlicer window", "elapsed_s": 0.4}
```

`text` is `waiting`, `waiting for an earlier confirmation to be answered`, `waiting for the user to
confirm in the OrcaSlicer window`, then `sending` / `uploading` (or the print job's own messages).

Finished:

```json
{"job": "job12", "kind": "print_send", "status": "finished", "percent": 100, "elapsed_s": 21.7,
 "result": {"printer": {"id": "prn_77d0e1c2a9", "name": "A1 Desk", "kind": "bambu", "connection": "lan", "model": "Bambu Lab A1"},
            "plate": 1, "time": "42m 10s", "time_s": 2530, "filament_g": 11.8, "layers": 50,
            "ams_mapping": [{"filament": 1, "filament_type": "PLA", "filament_color": "#FFFFFF",
                             "tray": "A1", "tray_type": "PLA", "tray_color": "#FFFFFF", "color_matches": true}],
            "options": {"bed_leveling": "off", "flow_calibration": "auto", "timelapse": true, "use_ams": true, "storage": "auto"},
            "warnings": [],
            "outcome": "sent", "message": "The printer accepted the job."}}
```

A host printer's result has `options: {start_print, file_name}` and no `ams_mapping`; its message
is `Uploaded; the host was asked to start printing.` or `Uploaded; not started.`. A request that
did not end in `sent` has `status` `failed` or `cancelled` and `result.error: {code, message,
hint?}` beside `outcome`:

| `outcome` | Job status | `error.code` | Meaning |
|---|---|---|---|
| `sent` | `finished` | | The printer accepted the job, or the host the upload. |
| `declined` | `failed` | `declined` | The user clicked Cancel, pressed Escape or closed the window. Nothing was sent. Do not send it again unless the user asks. |
| `timed_out` | `failed` | `timed_out` | Nobody answered within 5 minutes. Nothing was sent. Ask the user before trying again. |
| `cancelled` | `cancelled` | `cancelled` | The agent withdrew the request (`job_cancel`) before the user answered. |
| `not_sent` | `failed` | `printer_offline`, `printer_busy` | The printer changed while the request waited; nothing was sent. |
| `failed` | `failed` | `send_failed` (or `cancelled` if the print job was stopped) | The transfer failed; `message` says why. |

`job_cancel` on a request that is waiting for its turn ends it at once. On a request whose window
is open, the window stays until the user answers or it times out, the job keeps running until then,
and nothing is sent whatever the answer (outcome `cancelled`). A transfer that has started cannot be
called back.

## printer_control

Pauses, resumes or stops the print running on a printer. Needs `mcp_printer_send`. Marked
destructive; takes no `if_rev`.

| Argument | Type | Default | |
|---|---|---|---|
| `printer` | string | required | Id or exact name. |
| `action` | `pause` \| `resume` \| `stop` | required | |
| `wait_s` | number 0..280 | 10 | `stop`: seconds to wait for the user's answer. |

- Bambu Lab: the printer must be online (`printer_offline`) and live (`printer_status_unknown`:
  call `printer_status` first), and able to do the action now (`invalid_state`, `details` is the
  status: nothing is printing, already paused ...).
- Host printers: OctoPrint and Moonraker only; there is no state check before the command, so a
  command the host refuses is `control_failed`.
- `pause` and `resume` are sent at once, without asking the user, and return
  `{"ok": true, "printer": {...}, "action": "pause"}` (Bambu Lab adds a `note`). The printer
  reports the new state a few seconds later: check with `printer_status`. A command the printer or
  host does not take is `control_failed`.
- `stop` asks the user in the [confirmation window](#the-confirmation-window) first, goes through
  the same queue (`rate_limited`), and returns a job of kind `printer_control` like `print_send`.
  Its result is `{printer, action: "stop", outcome}` with outcome `stopped`, `declined`,
  `timed_out`, `cancelled`, `not_sent` (Bambu Lab: `invalid_state`, the print ended meanwhile) or
  `failed` (`control_failed`).

Journal: `control` (`{action, rc}` for Bambu Lab, `{action}` or `{action, ok}` for hosts),
`control_refused` (`{action, state}`) for `invalid_state`, and for `stop` `stop_requested`,
`stop_asked`, `stop_answer`.

## The confirmation window

Every `print_send` and every `stop` opens a window in the application and waits for the user,
unless the user switched on [Send print jobs without asking me](#access-switches-off-by-default).

What it shows:

- A bold line: "An AI agent asks to send a print job to this printer." (or "... asks to STOP the
  print on this printer."). Title `OrcaSlicer: confirm print job` / `OrcaSlicer: confirm stopping
  a print`.
- The plate picture, when the plate has one.
- Printer (name and model); Plate (`Plate 1 of "Box"`), or for a stop the Job name; Estimated
  time and Filament (grams); the connection (`LAN`, `Bambu cloud` or the host type); Bambu Lab: the
  options; host: the file name and whether printing starts; stop on a Bambu Lab printer: the
  progress.
- Bambu Lab: one row per filament, filament colour and type → tray colour, label and type, and
  "colour differs" where the colours are far apart.
- Every warning (colour differences, a compatible but different model, options the printer
  lacks).
- A countdown and the buttons **Send** (**Stop print**) and **Cancel**.

How it behaves:

| | |
|---|---|
| Send / Stop print | Disabled for the first second, so a click or key press meant for something else cannot send. |
| Cancel | The default and focused button: Enter never sends. |
| Escape, closing the window | Declined. |
| No answer | Closes after 5 minutes (300 s): `timed_out`. |
| Placement | Modeless, without a parent, stays on top and visible when the main window is minimised; it asks for attention (taskbar flash) when it opens. |
| One at a time | Further requests wait in the queue. |

The window is out of the agent's reach. It is modeless, so the dialog handling that answers the
application's dialogs during tool calls and jobs never sees it (and skips it by name if it ever
were modal); no tool can list, click or close it. Only the user's click, Escape, closing it, or the
timeout answers it.

## AMS mapping

Bambu Lab printers only. For every filament the plate uses (1-based project filament numbers),
which tray feeds it.

### Tray labels

| Label | Tray |
|---|---|
| `A1`..`A4`, `B1`..`B4`, `C1`..`C4`, `D1`..`D4` | AMS unit letter (first unit `A`), slot number 1-4. Further units continue with `E` ... |
| `HT1`, `HT2` ... | Single-slot units (AMS HT). |
| `ext` | The external spool holder. |
| `ext2` | The second external spool holder (two-nozzle printers). |

Labels are matched without regard to case. `printer_status` lists the trays with type, colour and
remaining percent.

### `"auto"` (default)

The application's own matching, as in the Print plate dialog: material type first, then the
nearest colour. With `options.use_ams: false`, or on a printer without an AMS, every filament goes
to `ext` (`tray_not_found` if the printer reports no external holder). If no tray fits:
`no_matching_tray` (`details.trays`).

### Explicit list

`[{"filament": 1, "tray": "A1"}, {"filament": 2, "tray": "A2"}]`, one entry for every filament the
plate uses.

| Problem | Code |
|---|---|
| A filament the plate does not use | `invalid_argument`, `details.used_filaments` |
| A used filament left out | `incomplete_mapping`, `details.trays` |
| A label the printer does not have | `tray_not_found`, `details.trays` |

### Checks on every mapping

| Situation | Result |
|---|---|
| The tray is empty | Refused: `tray_empty`. |
| The tray holds another material | Refused: `filament_type_mismatch`. Types are compared by their first word, ignoring case (`PLA Basic` is `PLA`); a filament without a type is not checked. |
| The colour differs (RGB distance above 60) | Not refused: a `warnings` entry, `color_matches: false` in the result's `ams_mapping`, "colour differs" in the window. The user decides. |

## Queue and rate limit

One confirmation window is open at a time. Further requests (`print_send` and `stop`) wait behind
it in order, at most three. One more request fails at once with `rate_limited`: wait for the user
to answer (`job_status`), then try again. A request whose turn comes is checked again first; if the
printer went offline or busy meanwhile it ends `not_sent` without a window. `app_status` →
`printers.sends_in_flight` counts requests waiting, being asked and being transferred.

## Journal entries

Printer tool calls write entries to the change journal that `history` lists (`{seq, time,
tool, scope, key, before, after, ...}`). `scope` is `{"printer": "<id>"}` (`null` for `list` and
`refused`), `before` is `null`, `after` holds the details. They cannot be reverted. Not journaled:
errors in the `plate` and `printer` arguments (`invalid_argument`, `stale_id`,
`printer_not_found`, `ambiguous_printer`), `rate_limited`, a host that does not answer `print_send`'s
check in time, and `printer_control`'s `printer_offline` / `printer_status_unknown`.

| `key` | Tool | `after` | When |
|---|---|---|---|
| `refused` | any of the four | `{reason: "printer_access_disabled", preference}` | A switch is off. |
| `list` | `printer_list` | `{count}` | Every call. |
| `status` | `printer_status` | `{}` | Every call. |
| `status_connect` | `printer_status` | `{}` | The tool selected the printer in the Device tab. |
| `send_refused` | `print_send` | `{reason, plate}` (host offline / busy: `{reason, state?}`) | A check refused the send. |
| `send_requested` | `print_send` | `{plate, summary}` (Bambu Lab) or `{plate, options}` (host) | The request was accepted and queued. |
| `send_asked`, `stop_asked` | `print_send`, `printer_control` | `{job}`, or `{job, unattended: true}` | The window opened, or (unattended) the request reached its turn. |
| `send_answer`, `stop_answer` | `print_send`, `printer_control` | `{answer: "confirmed" \| "declined" \| "timed_out", job}`, unattended adds `unattended: true` | The user answered, the window timed out, or (unattended) the request was taken as confirmed. |
| `send_result` | `print_send` | `{job, ok}` (Bambu Lab adds `status`) | The transfer ended. |
| `stop_requested` | `printer_control` | `{}` | A stop was queued. |
| `control` | `printer_control` | `{action, rc}` / `{action}` / `{action, ok}` | A pause, resume or stop command was sent. |
| `control_refused` | `printer_control` | `{action, state}` | `invalid_state` on a Bambu Lab printer. |

## How to use it as an agent

1. `app_status`: `printers.read_allowed` / `send_allowed` / `send_unattended`. If a switch you need is off, ask the user
   to switch it on in Preferences; do not look for a way around it.
2. `slice` the plate (`slice_status` until it is `sliced`).
3. `printer_list`: pick the printer by `id`; check `compatibility.compatible`.
4. `printer_status` on that printer, right before sending: Bambu Lab printers need a live status,
   and only one is live at a time. Read the trays if you map filaments yourself.
5. `print_send` with a small `wait_s` (0-10). Tell the user a confirmation window is waiting for
   them. When `send_unattended` is true there is no window and the plate goes out at once: send
   only what the user asked for.
6. `job_status` with `wait_s` until the job ends; report the `outcome`.
7. Never retry a `declined` or `timed_out` send on your own: that was the user's answer. Ask them.
   After `not_sent` or `failed`, read the error, fix the cause and ask before sending again.

The test-suite drives these tools against a fake printer agent that exists only in test builds
(CMake option `SLIC3R_MCP_TEST_PRINTERS`); release builds contain no such code.
