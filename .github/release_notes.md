OrcaSlicer **@APP_VERSION@** with the Bambu Linux bridge and the MCP server, built from `@TAG@` (commit `@SHA@`).
This is an unofficial build of [OrcaSlicer](https://github.com/OrcaSlicer/OrcaSlicer), not an OrcaSlicer or Bambu Lab release.

## What's included

- **Bambu Linux bridge (Windows x64).** The application does not load Bambu's Windows network plug-in. It
  loads a forwarder (`slicer_linux_runtime.dll`) that runs Bambu's **Linux** plug-in inside a small WSL 2
  distro of its own, `OrcaSlicer-BambuBridge`. Bambu's plug-in is not part of these downloads: OrcaSlicer
  downloads it from Bambu's servers when you install the network plug-in. The build also acknowledges and
  retries a print that A1 firmware rejects with error `050x-409D` when a BMCU is attached.
- **MCP server (Windows and macOS).** An AI agent on the same computer (Claude Code, Claude Desktop through
  `orca-mcp-bridge`) can inspect and edit the open project. It is **off by default**, listens on
  `127.0.0.1` only and needs an access token: *Preferences > General > MCP server (AI agent control)*.
- **Printer tools.** `printer_list`, `printer_status`, `print_send` and `printer_control` reach the printers
  OrcaSlicer already knows. Two switches in the same Preferences section enable them, and both are **off by
  default**. Every print sent and every print stopped also waits for your click in a confirmation window.

## Downloads

| File | For |
|---|---|
| `@SETUP@` | Windows 10/11 x64: **setup** (recommended) |
| `@ZIP@` | Windows 10/11 x64: portable zip |
| `@DMG@` | macOS 11+ on Apple Silicon (M1 or later) |

Each file has a `.sha256` next to it, and `SHA256SUMS.txt` lists them all. To check a download, compare the hash
with `certutil -hashfile <file> SHA256` on Windows or `shasum -a 256 <file>` on macOS.

## Windows

**WSL 2 is required for Bambu printers.** If it is not installed, run `wsl --install` in an administrator
terminal and restart Windows. The rest of OrcaSlicer works without it.

### Setup (recommended)

1. Run `@SETUP@`. The setup is **not code-signed**, so SmartScreen shows *"Windows protected your PC"*. Click
   **More info**, then **Run anyway**.
2. It installs for your account only, into `%LOCALAPPDATA%\Programs\OrcaSlicer-BambuBridge`, so no
   administrator rights are needed. It adds a Start menu entry and, optionally, a desktop shortcut.
3. Start **OrcaSlicer Bambu Bridge**. Your settings, presets and Bambu login are kept in the `data_dir` folder
   inside the installation, separate from any stock OrcaSlicer (`%APPDATA%\OrcaSlicer`).
4. In OrcaSlicer, install the Bambu network plug-in when it asks. The first time, it imports its WSL distro
   `OrcaSlicer-BambuBridge` (into `%LOCALAPPDATA%\OrcaSlicer-BambuBridge`), which takes up to a minute.

**Upgrading:** run the newer setup. It replaces the program files and keeps `data_dir`. Close OrcaSlicer
first; the setup waits until you do.

**Uninstalling:** use *Settings > Apps > Installed apps > OrcaSlicer Bambu Bridge*. The uninstaller asks
before it deletes your settings (`data_dir`); the default is to keep them. It never removes the WSL distro,
because another copy of this build may use it. To remove it yourself, run
`wsl --unregister OrcaSlicer-BambuBridge`.

**Moving from the portable zip:** with both copies closed, copy everything inside the portable copy's
`data_dir` folder into `%LOCALAPPDATA%\Programs\OrcaSlicer-BambuBridge\data_dir`.

**Another copy of this build on the same PC** (the portable zip, or one you built yourself) shares the
`OrcaSlicer-BambuBridge` WSL distro. If that copy imported a different root filesystem, the first plug-in
setup in this copy imports the distro again. The distro holds no settings, but close the other copy first.

### Portable zip

Unzip it anywhere you can write to, for example `Documents`, and run `orca-slicer.exe`. Settings stay in the
`data_dir` folder next to it. SmartScreen may warn the first time; click **More info**, then **Run anyway**.

## macOS (Apple Silicon)

1. Open `@DMG@` and drag **OrcaSlicer** onto **Applications**.
2. The app is **ad-hoc signed only**: it has no Apple Developer ID signature and is not notarized. Gatekeeper
   therefore blocks the first start of the downloaded copy ("cannot be opened" or "is damaged"). Allow it
   once in one of these ways:
   - In Terminal: `xattr -dr com.apple.quarantine /Applications/OrcaSlicer.app`, then open it normally.
   - On macOS 14 or earlier: right-click (or Control-click) OrcaSlicer in Applications, choose **Open**, then
     **Open** again.
   - On macOS 15 or later: open it once, then go to *System Settings > Privacy & Security* and click **Open
     Anyway**.

The Mac app is named `OrcaSlicer.app`, the same as a stock OrcaSlicer. It replaces a stock OrcaSlicer in
Applications and uses the same settings folder (`~/Library/Application Support/OrcaSlicer`). It has no Linux
bridge and loads Bambu's macOS plug-in, as stock OrcaSlicer does.
