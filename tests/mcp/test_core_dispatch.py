"""Main-thread dispatch: modal dialogs, re-entrancy, UI-busy rules, timeouts, jobs, undo, limits.

These use the debug_* tools (the instance runs with ORCA_MCP_DEBUG_TOOLS=1) to put the application
into the states the dispatcher has to respect.
"""

import threading
import time

from orca_mcp import McpClient


def in_thread(fn):
    """Run fn() in a thread; returns (thread, result holder)."""
    holder = {}

    def run():
        started = time.monotonic()
        try:
            holder["value"] = fn()
        except Exception as error:  # noqa: BLE001 - reported to the test
            holder["error"] = error
        holder["seconds"] = time.monotonic() - started

    thread = threading.Thread(target=run)
    thread.start()
    return thread, holder


# ---------------------------------------------------------------------------------------------
# Modal dialogs raised during a call
# ---------------------------------------------------------------------------------------------

def test_unanswered_dialog_fails_the_call_with_its_text(client):
    failure = client.tool_error("debug_dialog", kind="question", text="Convert the units?\nThe model looks tiny.")
    assert failure.code == "dialog_blocked"
    assert "Convert the units?" in failure.message
    dialog = failure.details["dialogs"][0]
    assert dialog["title"] == "MCP test question"
    assert dialog["message"] == "Convert the units?\nThe model looks tiny."
    # Non-destructive default: Cancel when the dialog has one.
    assert dialog["answered"] == "cancel"


def test_policy_answers_dialog_and_reports_it(client):
    for answer in ("yes", "no", "cancel"):
        result = client.tool("debug_dialog", kind="question", answer=answer, text="Proceed?")
        assert result["returned"] == answer
        assert result["dialogs"] == [{"title": "MCP test question", "message": "Proceed?", "answered": answer}]


def test_single_button_dialog_default_is_ok(client):
    result = client.tool("debug_dialog", kind="info", answer="default", text="Objects with zero volume removed")
    assert result["returned"] == "ok" and result["dialogs"][0]["answered"] == "ok"


def test_native_message_dialog_is_intercepted(client):
    result = client.tool("debug_dialog", kind="native", answer="no", text="Native question")
    assert result["returned"] == "no"
    assert result["dialogs"][0]["title"] == "MCP test native" and result["dialogs"][0]["message"] == "Native question"


def test_deferred_error_dialog_is_captured_in_the_call(client):
    # show_error() opens its dialog with CallAfter, i.e. after the code that called it returned.
    failure = client.tool_error("debug_dialog", kind="error", text="The file could not be loaded.")
    assert failure.code == "dialog_blocked"
    dialog = failure.details["dialogs"][0]
    assert "error" in dialog["title"].lower() and "The file could not be loaded." in dialog["message"]
    # Nothing is left on screen: the UI is idle for the next call.
    assert client.tool("app_status")["busy"]["modal_dialog"] is None


def test_non_informational_rule_still_fails_the_call(client):
    failure = client.tool_error("debug_dialog", kind="question", answer="cancel", informational=False)
    assert failure.code == "dialog_blocked" and failure.details["dialogs"][0]["answered"] == "cancel"


# ---------------------------------------------------------------------------------------------
# The user's own modal dialog
# ---------------------------------------------------------------------------------------------

def test_user_dialog_blocks_changes_but_not_reads(modern):
    modern.tool("debug_ui", action="modal", ms=4000)
    time.sleep(0.8)
    # Reading works while the dialog is up and says what is going on.
    status = modern.tool("app_status")
    assert status["busy"]["modal_dialog"] == "MCP test user dialog"
    assert "MCP test user dialog" in status["busy"]["blocks_changes"]
    assert modern.tool("scene_get", detail="summary")["plates"]
    # A change is refused once its queue time (1.5 s for debug_touch) is up, naming the dialog.
    started = time.monotonic()
    failure = modern.tool_error("debug_touch")
    waited = time.monotonic() - started
    assert failure.code == "ui_busy" and "MCP test user dialog" in failure.message
    assert 1.0 < waited < 4.0
    assert "MCP test user dialog" in failure.details["reason"]
    # After the dialog closed, changes run again.
    time.sleep(3.0)
    assert modern.tool("app_status")["busy"]["modal_dialog"] is None
    assert modern.tool("debug_touch")["ok"] is True


def test_change_waits_for_a_short_user_dialog(modern):
    modern.tool("debug_ui", action="modal", ms=900)
    time.sleep(0.4)
    started = time.monotonic()
    assert modern.tool("debug_touch")["ok"] is True  # queued, then run when the dialog closed
    assert time.monotonic() - started > 0.2


# ---------------------------------------------------------------------------------------------
# Re-entrancy: nothing starts inside another call or inside a yielding function
# ---------------------------------------------------------------------------------------------

def test_no_call_starts_while_the_app_is_yielding(app, modern):
    before = modern.tool("debug_stats")
    modern.tool("debug_ui", action="yield", ms=2000)
    time.sleep(0.5)  # the yielding loop has started
    started = time.monotonic()
    modern.tool("app_status")
    waited = time.monotonic() - started
    after = modern.tool("debug_stats")
    assert waited > 0.8, f"the call ran after {waited:.2f} s, inside the yielding function"
    assert after["blocked_yielding"] > before["blocked_yielding"]


def test_no_call_starts_inside_another_call(app, modern):
    other = McpClient(app.url, app.token, era="legacy").connect()
    before = modern.tool("debug_stats")
    # A handler that spins the event loop for 1.5 s (as GUI code with a progress dialog does) ...
    thread, first = in_thread(lambda: modern.tool("debug_touch", yield_inside=True, ms=1500))
    time.sleep(0.4)
    # ... while a second client calls. It must wait for the first call to finish.
    started = time.monotonic()
    other.tool("app_status")
    waited = time.monotonic() - started
    thread.join(20)
    assert "error" not in first, first.get("error")
    after = modern.tool("debug_stats")
    assert waited > 0.7, f"the second call ran after {waited:.2f} s, inside the first"
    assert after["blocked_reentrant"] > before["blocked_reentrant"]


def test_parallel_read_calls_all_complete(app):
    clients = [McpClient(app.url, app.token, era="modern" if i % 2 else "legacy").connect() for i in range(6)]
    threads = [in_thread(lambda c=c: c.tool("scene_get", detail="summary")) for c in clients]
    for thread, _ in threads:
        thread.join(30)
    for _, holder in threads:
        assert "error" not in holder, holder.get("error")
        assert holder["value"]["plates"]


# ---------------------------------------------------------------------------------------------
# Timeouts
# ---------------------------------------------------------------------------------------------

def test_run_timeout_answers_while_the_main_thread_is_stuck(app, modern):
    other = McpClient(app.url, app.token, era="modern")
    # debug_sleep blocks the main thread for 5 s; its run timeout is 1.5 s.
    thread, sleeper = in_thread(lambda: modern.tool_error("debug_sleep", ms=5000))
    time.sleep(0.5)
    # While it is stuck: the listener still answers (it never waits for the GUI) ...
    started = time.monotonic()
    assert other.rpc("tools/list")["tools"]
    assert time.monotonic() - started < 1.0
    # ... and a call that cannot start is refused when its queue time is up (0.5 s + 3 s grace).
    started = time.monotonic()
    failure = other.tool_error("debug_echo", payload=1)
    waited = time.monotonic() - started
    assert failure.code == "ui_busy" and failure.details["reason"] == "not_responding"
    assert 2.5 < waited < 4.6
    thread.join(20)
    assert sleeper["value"].code == "timeout"
    assert sleeper["seconds"] < 3.5, "the timeout answer must not wait for the handler"
    time.sleep(2.0)  # let the main thread come back
    assert modern.tool("app_status")["app"]


# ---------------------------------------------------------------------------------------------
# Jobs
# ---------------------------------------------------------------------------------------------

def test_job_finishes_within_the_wait(client):
    result = client.tool("debug_job", duration_s=1.0, wait_s=10)
    assert result["status"] == "finished" and result["result"] == {"waited_s": 1.0}
    assert result["job"].startswith("job") and result["percent"] == 100


def test_job_outlives_the_wait_then_is_polled(client):
    started = time.monotonic()
    result = client.tool("debug_job", duration_s=4.0, wait_s=1.0)
    assert result["status"] == "running" and 0.8 < time.monotonic() - started < 3.0
    job = result["job"]
    assert client.tool("job_status", job=job)["status"] == "running"  # wait_s 0: answers at once
    final = client.tool("job_status", job=job, wait_s=20)             # long poll
    assert final["status"] == "finished" and final["result"] == {"waited_s": 4.0}
    assert client.tool("job_status")["job"] == job  # most recent job


def test_job_failure_and_cancel(client):
    failed = client.tool("debug_job", duration_s=0.5, fail=True, wait_s=10)
    assert failed["status"] == "failed" and failed["result"]["error"]["code"] == "debug_failure"

    running = client.tool("debug_job", duration_s=30, wait_s=0.5)
    assert running["status"] == "running"
    client.tool("job_cancel", job=running["job"])
    final = client.tool("job_status", job=running["job"], wait_s=10)
    assert final["status"] == "cancelled"
    assert client.tool_error("job_cancel", job=running["job"]).code == "not_running"
    assert client.tool_error("job_status", job="job999999").code == "stale_id"


def test_other_calls_run_while_a_call_waits_for_a_job(app, modern):
    other = McpClient(app.url, app.token, era="modern")
    thread, waiting = in_thread(lambda: modern.tool("debug_job", duration_s=2.5, wait_s=10))
    time.sleep(0.5)
    started = time.monotonic()
    assert other.tool("app_status")["busy"]["jobs"], "the running job is reported"
    assert time.monotonic() - started < 1.5, "a waiting call must not hold the queue"
    thread.join(20)
    assert waiting["value"]["status"] == "finished"


# ---------------------------------------------------------------------------------------------
# Undo wrapper and revisions
# ---------------------------------------------------------------------------------------------

def test_snapshot_is_one_undo_step(modern):
    before = modern.tool("history")
    result = modern.tool("debug_touch", snapshot=True)
    assert result["undo"] == "MCP: debug touch"
    after = modern.tool("history")
    assert after["undo"][0] == "MCP: debug touch" and len(after["undo"]) == len(before["undo"]) + 1
    assert after["rev"] != before["rev"] and result["rev"] == after["rev"]
    undone = modern.tool("history", action="undo")
    assert undone["undone"] == ["MCP: debug touch"] and undone["redo"][0] == "MCP: debug touch"
    assert modern.tool("history")["rev"] == before["rev"]


def test_mutating_tool_without_snapshot_adds_no_undo_step(modern):
    before = modern.tool("history")
    result = modern.tool("debug_touch")
    assert "undo" not in result
    assert modern.tool("history")["undo"] == before["undo"]


def test_if_rev_conflict(modern):
    rev = modern.tool("history")["rev"]
    assert modern.tool("debug_touch", if_rev=rev)["ok"] is True
    failure = modern.tool_error("debug_touch", if_rev=rev + 12345)
    assert failure.code == "conflict" and failure.details["rev"] == rev


# ---------------------------------------------------------------------------------------------
# Result limits and notes
# ---------------------------------------------------------------------------------------------

def test_oversized_result_is_refused(client):
    assert client.tool("debug_echo", size=100000)["filler"] == "x" * 100000
    failure = client.tool_error("debug_echo", size=500000)
    assert failure.code == "too_large" and "limit" in failure.hint


def test_notes_are_returned(client):
    assert client.tool("debug_echo", note="closed the gizmo")["notes"] == ["closed the gizmo"]


# ---------------------------------------------------------------------------------------------
# Journal: changes outside the undo stack
# ---------------------------------------------------------------------------------------------

def test_journal_records_and_reverts(modern):
    start = modern.tool("debug_touch")["value"]
    first = modern.tool("debug_touch", set_value="one")
    second = modern.tool("debug_touch", set_value="two")
    assert len(first["journal"]) == 1 and second["journal"][0] == first["journal"][0] + 1
    journal = modern.tool("history")["journal"]
    assert journal[0]["seq"] == second["journal"][0], "newest first"
    assert journal[0]["tool"] == "debug_touch" and journal[0]["key"] == "value"
    assert journal[0]["before"] == "one" and journal[0]["after"] == "two" and journal[0]["revertible"] is True
    reverted = modern.tool("history", action="revert", journal_seq=first["journal"] + second["journal"])
    assert reverted["reverted"] == second["journal"] + first["journal"], "reverted newest first"
    assert modern.tool("debug_touch")["value"] == start
    assert modern.tool_error("history", action="revert", journal_seq=first["journal"]).code == "already_reverted"
    assert modern.tool_error("history", action="revert", journal_seq=[987654]).code == "stale_id"
    assert modern.tool_error("history", action="revert").code == "invalid_argument"
