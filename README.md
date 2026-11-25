# OrcaSlicer 2.5.1 + Bambu Linux bridge (private fork)

This is [OrcaSlicer](https://github.com/OrcaSlicer/OrcaSlicer) **2.5.1** with a "Bambu Linux bridge": the
application does not load Bambu's native network plug-in but a forwarder module (`slicer_linux_runtime.dll`
on Windows x64, `libslicer_linux_runtime.dylib` on macOS arm64) that runs Bambu's **Linux** plug-in (series
**02.08.04**) inside a small dedicated WSL2 distro on Windows, or an OrbStack Linux machine on macOS. Bambu's plug-in is not part of this repository or of any build; the application
downloads it from Bambu's servers when the network plug-in is installed. The fork also acknowledges and
retries a print that A1 firmware rejects with error `050x-409D` when a BMCU is attached. Everything below
the line is upstream's README.

## Download and install

Installers are on this repository's **Releases** page (*Releases* in the sidebar, or
`https://github.com/Bl4ckBl1zZ/OrcaSlicer-BambuBridge/releases/latest`). The repository is private, so only
accounts with access to it can open the page and download. Each release has:

| File | For |
|---|---|
| `OrcaSlicer_BambuBridge_Windows_x64_<tag>_setup.exe` | Windows 10/11 x64: setup (recommended) |
| `OrcaSlicer_BambuBridge_Windows_x64_<tag>_portable.zip` | Windows 10/11 x64: portable |
| `OrcaSlicer_BambuBridge_macOS_arm64_<tag>.dmg` | macOS 11+ on Apple Silicon |

plus a `.sha256` for each file and `SHA256SUMS.txt`. The release notes repeat the steps below.

**Windows setup.** It is not code-signed, so SmartScreen says *"Windows protected your PC"*: click
**More info**, then **Run anyway**. It installs for the current user into
`%LOCALAPPDATA%\Programs\OrcaSlicer-BambuBridge`, without administrator rights, and adds a Start menu entry
*OrcaSlicer Bambu Bridge*, an optional desktop shortcut and an entry in *Settings > Apps*. Settings, presets
and the Bambu login live in the `data_dir` folder of the installation, the same way as in the portable zip.
That keeps them apart from a stock OrcaSlicer in `%APPDATA%\OrcaSlicer`, whose network plug-in folder this
build would otherwise replace. A newer setup installs over an older one and keeps `data_dir`. The
uninstaller keeps `data_dir` unless you agree to delete it. It never removes the WSL distro; to remove that
yourself, run `wsl --unregister OrcaSlicer-BambuBridge`. Source: `tools/windows_installer/` (NSIS).

**Windows portable zip.** Unzip it anywhere you can write to and run `orca-slicer.exe`. Settings stay in
`data_dir` next to it.

**WSL 2 is required for Bambu printers on Windows.** If it is missing, run `wsl --install` in an
administrator terminal and restart Windows. The first time the Bambu network plug-in is installed,
OrcaSlicer imports its own distro `OrcaSlicer-BambuBridge` into `%LOCALAPPDATA%\OrcaSlicer-BambuBridge`. All
copies of this build on one PC share that distro.

**macOS.** Open the DMG and drag OrcaSlicer onto Applications. It is Apple Silicon only, **ad-hoc signed,
with no Apple Developer ID and no notarization**, so Gatekeeper blocks the first start. Run
`xattr -dr com.apple.quarantine /Applications/OrcaSlicer.app` once. Instead, you can right-click the app and
choose **Open** (macOS 14 and earlier), or click **Open Anyway** in *System Settings > Privacy & Security*
(macOS 15 and later). The app is named `OrcaSlicer.app` and shares its settings with a stock OrcaSlicer.

**OrbStack is required for the bridge on macOS.** Install [OrbStack](https://orbstack.dev) and start it once.
Without OrbStack the app uses Bambu's macOS plug-in, as stock OrcaSlicer does. With it, the first install of
the Bambu network plug-in creates an arm64 Ubuntu 26.04 machine `orca-bambu-bridge`, installs QEMU in it and
unpacks an x86-64 root filesystem to `/opt/x64root`. Bambu's plug-in is x86-64 only and crashes under
Rosetta, so the host runs through `qemu-x86_64`. Because of the emulation, loading the plug-in makes the
start of OrcaSlicer about 20 seconds slower. To remove the machine, run `orb delete orca-bambu-bridge`. The
machine sits behind OrbStack's NAT; LAN discovery of printers through it is untested. To use Bambu's macOS
plug-in although OrbStack is installed, start OrcaSlicer with `SLICER_LINUX_RUNTIME_ENABLED=0` in the environment.

## Builds

**Builds** come from the `Build` workflow (`.github/workflows/build.yml`), which runs only on demand and
for version tags:

- *Actions > Build > Run workflow* builds Windows x64 and/or macOS arm64. The results are artifacts of the
  run: `windows-x64-setup`, `windows-x64-portable` and `macos-arm64-dmg`. Nothing is released. Run it once
  from `main` first: that fills the dependency caches every later run (tags included) restores.
- Pushing a tag `v<version>-<suffix>` (the version from `version.inc`, for example `v2.5.1-bambu.1`) builds
  both platforms and **publishes** a release for that tag with the files above. The tag must be new to this
  fork: never push `v2.5.1` itself, which is upstream's tag. To release:
  `git tag v2.5.1-bambu.2 && git push private refs/tags/v2.5.1-bambu.2`.
- In CI, the Windows setup is installed silently into a scratch folder, checked with
  `verify_windows_package.ps1`, upgraded over itself and uninstalled before it is uploaded.

**Windows build time.** GitHub's Windows runner for a private repository has 2 processors and 8 GB. A
run that finds nothing cached needs over four hours: about 2¼ h for the dependencies (the `Windows
dependencies` job) and about 2¼ h to compile the slicer. Both are cached from `main` (GitHub drops an entry
unused for a week), and a run that finds them takes about 15 minutes: the slicer compiles with Visual Studio
2022's cl through Ninja and ccache, and only linking and packing take time. A change to `deps/` rebuilds
the dependencies; a change to a header that many files include recompiles most of the slicer. The compiler
cache is saved even when the build fails or runs out of time, so the next run carries on from there.

**Linux job on a self-hosted runner.** The `linux-runtime` job (the bridge's Linux host and the WSL rootfs,
used by both the Windows and the macOS build)
runs on GitHub's `ubuntu-24.04` unless the repository variable `LINUX_RUNNER` (*Settings > Secrets and
variables > Actions > Variables*) holds a runner label, for the graysmith runners `graysmith-4cpu`. Windows
and macOS always build on GitHub's runners. If graysmith is down, the job is never picked up and the run
sits queued (the job's 30-minute timeout only counts once a runner has taken it): cancel the run, delete
the variable and start it again. *Run workflow* with only the Linux runtime option ticked tries the runner without
starting a Windows build. The job installs its own packages with `apt-get` and needs neither Docker nor
`sudo` when it runs as root. Its logic is `tools/slicer_linux_runtime/ci/build_linux_runtime.sh`, which
also runs outside Actions.

---

<div align="center">

<picture>
  <img alt="OrcaSlicer logo" src="resources/images/OrcaSlicer.png" width="15%" height="15%">
</picture>

<a href="https://trendshift.io/repositories/15552" target="_blank"><img src="https://trendshift.io/api/badge/repositories/15552" alt="OrcaSlicer%2FOrcaSlicer | Trendshift" style="width: 250px; height: 55px;" width="250" height="55"/></a>

[![GitHub Repo stars](https://img.shields.io/github/stars/OrcaSlicer/OrcaSlicer)](https://github.com/OrcaSlicer/OrcaSlicer/stargazers) [![Build all](https://img.shields.io/github/actions/workflow/status/OrcaSlicer/OrcaSlicer/main_build_status.yml?label=Build%20all)](https://github.com/OrcaSlicer/OrcaSlicer/actions/workflows/build_all.yml)

OrcaSlicer: an open source Next-Gen Slicing Software for Precision 3D Prints.  
Optimize your prints with ultra-fast slicing, intelligent support generation, and seamless printer compatibility—engineered for perfection.

# Official links and community

#### Official Website:

<a href="https://www.orcaslicer.com/" style="font-size:2em;">OrcaSlicer.com</a>

#### Github Repository:

<a href="https://github.com/OrcaSlicer/OrcaSlicer"><img src="https://img.shields.io/badge/OrcaSlicer-181717?style=flat&logo=github&logoColor=white" width="200" alt="GitHub Logo"/> </a>

#### Follow us:

<a href="https://twitter.com/real_OrcaSlicer"><img src="https://img.shields.io/badge/real__OrcaSlicer-000000?style=flat&logo=x&logoColor=white" width="200" alt="X Logo"/> </a>  
<a href="https://www.youtube.com/@OfficialOrcaSlicer"><img src="https://img.shields.io/badge/OfficialOrcaSlicer-FF0000?style=flat&logo=youtube&logoColor=white" width="200" alt="YouTube Logo"/> </a>

#### Join our Discord community:

<a href="https://discord.gg/P4VE9UY9gJ"><img src="https://img.shields.io/badge/-Discord-5865F2?style=flat&logo=discord&logoColor=fff" width="200" alt="discord logo"/> </a>

<table border="2" style="border-color: #ffa500; background-color:rgb(232, 220, 180); color: #856404;">
<tr>
<td>
<strong>⚠️ CAUTION:</strong><br>
Several clickbait and malicious websites, such as <b>orca-slicer[.]com</b> and <b>orcaslicer[.]net</b>, are pretending to be the official OrcaSlicer site. These sites may redirect you to dangerous downloads or contain misleading information.<br>
<b>Our only official website is <a href="https://www.orcaslicer.com/">www.orcaslicer.com</a>.</b><br><br>
If you come across any of these in search results, please <b>report them</b> as unsafe or phishing to help keep the community secure with:<br>
 - <a href="https://safebrowsing.google.com/safebrowsing/report_phish/">Google Safe Browsing</a><br>
 - <a href="https://www.microsoft.com/en-us/wdsi/support/report-unsafe-site">Microsoft Security Intelligence</a><br>
 - <a href="https://ipthreat.net/tools/reportphishing">IPThreat</a>
</td>
</tr>
</table>

</div>

# Main features

- **[Advanced Calibration Tools](https://www.orcaslicer.com/wiki/calibration_guide)**  
  Comprehensive suite: temperature towers, flow rate, retraction & more for optimal performance.
- **[Precise Wall](https://www.orcaslicer.com/wiki/quality_settings_precision#precise-wall) and [Seam Control](https://www.orcaslicer.com/wiki/quality_settings_seam)**  
  Adjust outer wall spacing and apply scarf seams to enhance print accuracy.
- **[Sandwich Mode](https://www.orcaslicer.com/wiki/quality_settings_wall_and_surfaces#innerouterinner) and [Polyholes](https://www.orcaslicer.com/wiki/quality_settings_precision#polyholes) Support**  
  Use varied infill [patterns](https://www.orcaslicer.com/wiki/strength_settings_patterns) and accurate hole shapes for improved clarity.
- **[Overhang](https://www.orcaslicer.com/wiki/quality_settings_overhangs) and [Support Optimization](https://www.orcaslicer.com/wiki#support-settings)**  
  Modify geometry for printable overhangs with precise support placement.
- **[Granular Controls and Customization](https://www.orcaslicer.com/wiki#process-settings)**  
  Fine-tune print speed, layer height, pressure, and temperature with precision.
- **Network Printer Support**  
  Seamless integration with Klipper, PrusaLink, and OctoPrint for remote control.
- **[Mouse Ear Brims](https://www.orcaslicer.com/wiki/others_settings_brim) & [Adaptive Bed Mesh](https://www.orcaslicer.com/wiki/printer_basic_information_adaptive_bed_mesh)**  
  Automatic brims and adaptive mesh calibration ensure consistent adhesion.
- **User-Friendly Interface**  
  Intuitive drag-and-drop design with pre-made profiles for popular printers.
- **[Open-Source](https://github.com/OrcaSlicer/OrcaSlicer) & [Community Driven](https://discord.gg/P4VE9UY9gJ)**  
  Regular updates fueled by continuous community contributions.
- **Wide Printer Compatibility**  
  Supports a broad range of printers: Bambu Lab, Prusa, Creality, Voron, and more.
- **[Belt Printer Support](https://www.orcaslicer.com/wiki/belt_printing)**  
  Slice for belt / conveyor (infinite-Z) printers, with belt-aware supports and a tilted-bed preview. Contributed by [Joseph Robertson (@HarrierPigeon)](https://github.com/HarrierPigeon).
- **[IDEX/IQEX Parallel Printing Support](https://www.orcaslicer.com/wiki/idex_iqex_parallel_printing)**  
  Print copies or mirror images of a part on every carriage of an IDEX or IQEX printer at once, with the mode chosen per plate and nozzle clearance zones shown on the bed. Contributed by [Clifford (@cgarwood82)](https://github.com/cgarwood82).
- Additional features can be found in the [change notes](https://github.com/OrcaSlicer/OrcaSlicer/releases/).

# Wiki

The [wiki](https://www.orcaslicer.com/wiki) aims to provide a detailed explanation of the slicer settings, including how to maximize their use and how to calibrate and set up your printer.

- **[Access the wiki here](https://www.orcaslicer.com/wiki)**
- **[Contribute to the wiki](https://www.orcaslicer.com/wiki/how_to_wiki)**

# Download

## Stable Release

📥 **[Download the Latest Stable Release](https://github.com/OrcaSlicer/OrcaSlicer/releases/latest)**  
Visit our GitHub Releases page for the latest stable version of OrcaSlicer, recommended for most users.  
Windows and macOS release and nightly builds are code signed.

## Nightly Builds

🌙 **[Download the Latest Nightly Build](https://github.com/OrcaSlicer/OrcaSlicer/releases/tag/nightly-builds)**  
Explore the latest developments in OrcaSlicer with our nightly builds. Feedback on these versions is highly appreciated.

# How to install

## Windows

Download the **Windows Installer exe** for your preferred version from the [releases page](https://github.com/OrcaSlicer/OrcaSlicer/releases). Both `x64` and `arm64` installers are published — pick the one matching your CPU.

- *For convenience there is also a portable build available.*
    <details>
    <summary>Troubleshooting</summary>

  - *If you have troubles to run the build, you might need to install following runtimes:*
  - [MicrosoftEdgeWebView2RuntimeInstallerX64](https://github.com/OrcaSlicer/OrcaSlicer/releases/download/v1.0.10-sf2/MicrosoftEdgeWebView2RuntimeInstallerX64.exe)
    - [Details of this runtime](https://aka.ms/webview2)
    - [Alternative Download Link Hosted by Microsoft](https://go.microsoft.com/fwlink/p/?LinkId=2124703)
  - [vcredist2019_x64](https://github.com/OrcaSlicer/OrcaSlicer/releases/download/v1.0.10-sf2/vcredist2019_x64.exe)
    - [Alternative Download Link Hosted by Microsoft](https://aka.ms/vs/17/release/vc_redist.x64.exe)
    - This file may already be available on your computer if you've installed visual studio.  Check the following location: `%VCINSTALLDIR%Redist\MSVC\v142`
    </details>

### Microsoft Store

Install from the [Microsoft Store](https://apps.microsoft.com/detail/9mv6gl23xm59) when you prefer a Store-signed package (helps on Windows 11 Smart App Control).

### Windows Package Manager

```shell
winget install --id=SoftFever.OrcaSlicer -e
```

## Mac

1. Download the universal DMG, which runs on both Apple Silicon and Intel Macs.
2. Drag OrcaSlicer.app to Application folder.
3. *If you want to run a build from a PR, you also need to follow the instructions below:*

    <details>
    <summary>Quarantine</summary>

    - Option 1 (You only need to do this once. After that the app can be opened normally.):
      - Step 1: Hold _cmd_ and right click the app, from the context menu choose **Open**.
      - Step 2: A warning window will pop up, click _Open_

    - Option 2:
      Execute this command in terminal:

      ```shell
      xattr -dr com.apple.quarantine /Applications/OrcaSlicer.app
      ```

    - Option 3:
        - Step 1: open the app, a warning window will pop up  
            ![mac_cant_open](./SoftFever_doc/mac_cant_open.png)
        - Step 2: in `System Settings` -> `Privacy & Security`, click `Open Anyway`:  
            ![mac_security_setting](./SoftFever_doc/mac_security_setting.png)
    </details>

### Homebrew Cask

```shell
brew install --cask orcaslicer
```

The [Homebrew cask](https://formulae.brew.sh/cask/orcaslicer) installs the official macOS DMG from [GitHub Releases](https://github.com/OrcaSlicer/OrcaSlicer/releases).

## Linux

### Flathub (Recommended)

OrcaSlicer is available through FlatHub:

<a href='https://flathub.org/apps/com.orcaslicer.OrcaSlicer'><img width='240' alt='Download on Flathub' src='https://dl.flathub.org/assets/badges/flathub-badge-en.png'/></a>

Install from the command line:

```shell
flatpak install flathub com.orcaslicer.OrcaSlicer
flatpak run com.orcaslicer.OrcaSlicer
```

It can also be installed through graphical software managers (KDE Discover, GNOME Software, etc.) when Flathub is enabled. Search for **OrcaSlicer** in your software center.

### AppImage

AppImages are published for both **x86_64** and **aarch64** (ARM64). Pick the file matching your CPU — the ARM64 build has `aarch64` in its name (e.g. `OrcaSlicer_Linux_AppImage_Ubuntu2404_aarch64_*.AppImage`).

 1. Download App image from the [releases page](https://github.com/OrcaSlicer/OrcaSlicer/releases).
 2. Double click the downloaded file to run it.

 3. If you run into trouble executing it, try this command in the terminal:
    `chmod +x /path_to_appimage/OrcaSlicer_Linux.AppImage`

# How to Compile

All updated build instructions for Windows, macOS, and Linux are now available on the official [OrcaSlicer Wiki - How to build](https://www.orcaslicer.com/wiki/how_to_build) page.

Please refer to the wiki to ensure you're following the latest and most accurate steps for your platform.

# Klipper Note

If you're running Klipper, it's recommended to add the following configuration to your `printer.cfg` file.

```gcode
# Enable object exclusion
[exclude_object]

# Enable arcs support
[gcode_arcs]
resolution: 0.1
```

# Supports

**OrcaSlicer** is an open-source project, and we're deeply grateful to all our sponsors and backers.  
Their generous support helps fund filaments and other essential 3D printing materials for the project.  
Thank you! :)

## Sponsors

<table>
<tr>
<td>
<a href="https://qidi3d.com/" style="display:inline-block; border-radius:8px; background:#fff;">
  <img src="SoftFever_doc\sponsor_logos\QIDI.png" alt="QIDI" width="100" height="100">
</a>
</td>
<td>
<a href="https://bigtree-tech.com/" style="display:inline-block; border-radius:8px; background:#222;">
    <img src="SoftFever_doc\sponsor_logos\BigTreeTech.png" alt="BIGTREE TECH" width="100" height="100">
</a>
</td>
</tr>
</table>

## Backers:

**Ko-fi supporters** ☕: [Backers list](https://github.com/user-attachments/files/16147016/Supporters_638561417699952499.csv)

## Support the project

<a href="https://github.com/sponsors/SoftFever"><img src="https://img.shields.io/badge/GitHub%20Sponsors-30363D?style=flat&logo=GitHub-Sponsors&logoColor=EA4AAA" height="50"></a>  
<a href="https://ko-fi.com/G2G5IP3CP"><img src="https://img.shields.io/badge/Support_me_on_Ko--fi-FF5E5B?style=flat&logo=ko-fi&logoColor=white" height="50"></a>  
<a href="https://paypal.me/softfever3d"><img src="https://img.shields.io/badge/PayPal-003087?style=flat&logo=paypal&logoColor=fff" height="50"></a>

## Some Background

Open-source slicing has always been built on a tradition of collaboration and attribution. [Slic3r](https://github.com/Slic3r/Slic3r), created by Alessandro Ranellucci and the RepRap community, laid the foundation. [PrusaSlicer](https://github.com/prusa3d/PrusaSlicer) by Prusa Research built on Slic3r and acknowledged that heritage. [Bambu Studio](https://github.com/bambulab/BambuStudio) in turn forked from PrusaSlicer, and [SuperSlicer](https://github.com/supermerill/SuperSlicer) by @supermerill extended PrusaSlicer with community-driven enhancements. Each project carried the work of its predecessors forward, crediting those who came before.

OrcaSlicer began in that same spirit, drawing from BambuStudio, PrusaSlicer, and ideas inspired by CuraSlicer and SuperSlicer. But it has since grown far beyond its origins. Through relentless innovation — introducing advanced calibration tools, precise wall and seam control, tree supports, adaptive slicing, and hundreds of other features — OrcaSlicer has become the most widely used and actively developed open-source slicer in the 3D printing community. Many of its innovations have been adopted by other slicers, making it a driving force for the entire industry.

The OrcaSlicer logo was designed by community member [Justin Levine](https://github.com/jal-co).

# License

- **OrcaSlicer** is licensed under the GNU Affero General Public License, version 3.
- The **GNU Affero General Public License**, version 3 ensures that if you use any part of this software in any way (even behind a web server), your software must be released under the same license.
- OrcaSlicer includes a **pressure advance calibration pattern test** adapted from Andrew Ellis' generator, which is licensed under GNU General Public License, version 3. Ellis' generator is itself adapted from a generator developed by Sineos for Marlin, which is licensed under GNU General Public License, version 3.
- The **Bambu networking plugin** is based on non-free libraries from BambuLab. It is optional to the OrcaSlicer and provides extended functionalities for Bambulab printer users.
