"""Test harness for the print confirmation window (src/slic3r/Mcp/tools/printers/SendConfirm.cpp).

The window asks the user before an AI agent's print job is sent. No MCP tool can answer it, by
design, so the tests answer it the way a person does: through the operating system. This module
finds the window of one OrcaSlicer process by its title, reads its texts and clicks its native
buttons (WM_COMMAND / BN_CLICKED to the dialog, exactly what a mouse click on the button produces).

Windows only. Only windows of the process id given are touched.
"""

from __future__ import annotations

import sys
import time

TITLE_PREFIX = "OrcaSlicer: confirm"

if sys.platform == "win32":
    import ctypes
    import ctypes.wintypes as wt

    _user32 = ctypes.windll.user32
    _EnumProc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    _user32.SendMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
    _user32.SendMessageW.restype = ctypes.c_ssize_t
    _user32.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
    _user32.GetDlgCtrlID.argtypes = [wt.HWND]

WM_COMMAND = 0x0111
WM_CLOSE = 0x0010
BN_CLICKED = 0


def _text(hwnd) -> str:
    n = _user32.GetWindowTextLengthW(hwnd)
    buf = ctypes.create_unicode_buffer(n + 1)
    _user32.GetWindowTextW(hwnd, buf, n + 1)
    return buf.value


def _class(hwnd) -> str:
    buf = ctypes.create_unicode_buffer(256)
    _user32.GetClassNameW(hwnd, buf, 256)
    return buf.value


def _pid_of(hwnd) -> int:
    pid = wt.DWORD()
    _user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
    return pid.value


def find(pid: int):
    """The visible confirmation window of process `pid`, or None."""
    found = []

    @_EnumProc
    def visit(hwnd, _):
        if _pid_of(hwnd) == pid and _user32.IsWindowVisible(hwnd) and _text(hwnd).startswith(TITLE_PREFIX):
            found.append(hwnd)
        return True

    _user32.EnumWindows(visit, 0)
    return found[0] if found else None


def wait(pid: int, timeout: float = 20.0):
    """Waits for the confirmation window of `pid`; raises TimeoutError."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        hwnd = find(pid)
        if hwnd:
            return hwnd
        time.sleep(0.1)
    raise TimeoutError(f"no confirmation window of process {pid} within {timeout:.0f} s")


def wait_gone(pid: int, timeout: float = 10.0) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if not find(pid):
            return True
        time.sleep(0.1)
    return False


def children(hwnd) -> list:
    out = []

    @_EnumProc
    def visit(child, _):
        out.append(child)
        return True

    _user32.EnumChildWindows(hwnd, visit, 0)
    return out


def texts(hwnd) -> list[str]:
    """Title plus the texts of all child windows (labels and buttons), in window order."""
    return [_text(hwnd)] + [t for t in (_text(c) for c in children(hwnd)) if t]


def button(hwnd, label: str):
    for child in children(hwnd):
        if _class(child).lower() == "button" and _text(child).replace("&", "") == label:
            return child
    raise LookupError(f"no button {label!r} in the confirmation window; texts: {texts(hwnd)}")


def click(hwnd, label: str, timeout: float = 5.0) -> None:
    """Clicks the button `label` ("Send", "Stop print", "Cancel") once it is enabled."""
    btn = button(hwnd, label)
    deadline = time.monotonic() + timeout
    while not _user32.IsWindowEnabled(btn):
        if time.monotonic() > deadline:
            raise TimeoutError(f"button {label!r} stayed disabled")
        time.sleep(0.1)
    ctrl_id = _user32.GetDlgCtrlID(btn)
    wparam = (BN_CLICKED << 16) | (ctrl_id & 0xFFFF)
    _user32.SendMessageW(hwnd, WM_COMMAND, wparam, btn)


def is_enabled(hwnd, label: str) -> bool:
    return bool(_user32.IsWindowEnabled(button(hwnd, label)))


def close(hwnd) -> None:
    """Closes the window with its title-bar X (WM_CLOSE)."""
    _user32.PostMessageW(hwnd, WM_CLOSE, 0, 0)


def answer(pid: int, label: str, timeout: float = 20.0) -> list[str]:
    """Waits for the window, clicks `label`, waits for it to go; returns the texts it showed."""
    hwnd = wait(pid, timeout)
    shown = texts(hwnd)
    click(hwnd, label)
    assert wait_gone(pid), "the confirmation window stayed open after the click"
    return shown
