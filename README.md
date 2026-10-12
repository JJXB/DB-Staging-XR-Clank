# DOSBox Staging XR (Clanker Fork)

![GPL-2.0-or-later][gpl-badge]
[![Chat][discord-badge]][discord]
---
**AI Disclaimer:**
I Built this not to take away from the work of people but to instead build the dosbox version with features i had been wishing for people to add and integrate for a long time. If anyone decides to make this repo obsolete either through forking or rewriting the XR elements from scratch, feel welcome.
---
**XR Additions**
- Stereo Rendering (Mono, Anaglyph, Aternating, Interleaved, TTB, SBS, WIP Voodoo Depth, Auto Depth From 2D WIP)
- Picture lock in one position or following headset
- (Optional) Neural 2D Depth Generation as a build flag
-Keyboard and Mouse Input via Motion Controls
- In-XR Menu
- Toasts for Important XR Toggles/options in both XR and Desktop.

**Todo**
- Depth extraction and projection from voodoo games (Carmageddon 0x6 error, screamer 2 ef2k no depth buffer.)
- Improve XR Menu
- COM1 Tracker
- Cyberglasses Format
- (Long Term) virtuality 2000
  
**Key Shortcuts**
- Ctrl+Alt+E Autodepth mode 
- Ctrl+Alt+T Mirror Stereo Mode 
- Ctrl+E Toggle Selected Stereo mode
- Ctrl+T Enable/Disable Stereo Mode 
- Ctrl+D Anaglyph method
- Ctrl+Alt+D Neural Model
- Ctrl+Alt+W Shutter source 
- Ctrl+W Eye swap 
- Ctrl+=/− Picture size 
- Ctrl+Alt+[/] HMD Disparity 
- Ctrl+[/] HMD Frame Divisor 
- Ctrl+0 Picture Recenter 
- Ctrl+Alt+0 Picture Lock To Headset
- Ctrl+M Controller Mouse
- Ctrl+Alt+M keyboard
- Ctrl+Alt+G Depth VFlip
- Ctrl+;/' Depth Strength
- Ctrl+G Depth Debug view. 

**New options in dosbox-staging.conf**
```
[xr]
# Screen / picture placement
# xr_screen_distance: cm, 50-500
xr_screen_distance = 200
# xr_picture_size: % of eye texture, 20-100
xr_picture_size = 35
# xr_headset_picture_lock: glue picture to headset vs room-lock
xr_headset_picture_lock = true

# Stereo input: mono, anaglyph, alternating, cybershades, interleaved,
# top-bottom, sbs, depth, autodepth, cybermaxx, iglasses
xr_stereo_input = mono
# xr_anaglyph_method: simple, dubois
xr_anaglyph_method = simple
xr_eye_swap = false
# xr_disparity: px, -128..128
xr_disparity = 0
# xr_shutter_sync: auto, parity, lpt
xr_shutter_sync = auto
# xr_submit_divisor: 1-8
xr_submit_divisor = 1

# Depth synthesis (depth/autodepth modes)
# xr_depth_strength: %, 0-50
xr_depth_strength = 4
xr_flip_vertical = false
# xr_show_depth: grayscale depth debug view
xr_show_depth = false
# xr_autodepth_method: blend, gradient, luma, neural,
# neural+gradient, neural+luma, neural+blend
xr_autodepth_method = blend
xr_autodepth_invert = false
# Neural depth (needs XRDEPTH DOWNLOAD + ONNX Runtime)
# xr_neural_interval_ms: 16-5000
xr_neural_interval_ms = 16
# xr_neural_model: midas, da2int8, fastdepth, lightdepth
xr_neural_model = midas
# xr_neural_threads = 0  # 0=auto (max 4), 1-16 manual, restart to apply
xr_neural_threads = 4

# VR menu (menu button; sticks/A-B also work inside)
# xr_menu_scale: %, 25-200
xr_menu_scale = 80
# xr_menu_distance: cm, 50-500
xr_menu_distance = 180
# xr_menu_offset_y: % of half-height, -100..100
xr_menu_offset_y = -60
# xr_menu_font_size: %, 50-200
xr_menu_font_size = 60

# Overlay keyboard (grip+A / Ctrl+Alt+M)
# xr_kb_distance: cm, 50-500
xr_kb_distance = 170
# xr_keyboard_scale: %, 25-200
xr_keyboard_scale = 60
# xr_keyboard_key_scale: %, 50-200 (crisper keys)
xr_keyboard_key_scale = 60
# xr_keyboard_offset_x: %, -100..100
xr_keyboard_offset_x = 0
# xr_keyboard_offset_y: %, -100..100
xr_keyboard_offset_y = 20
# xr_keyboard_opacity: %, 10-100
xr_keyboard_opacity = 100

# Notifications / toast overlay live in the [osd] section below.

# Horizontal sensitivity of the controller-aimed mouse as a percentage.
# 100 means raw motion. Scales relative movement only, so screen
# edges stay reachable. Valid range from 10 to 400.
xr_controller_mouse_sense_x = 100
# Vertical sensitivity of the controller-aimed mouse as a percentage.
# 100 means raw motion. Valid range from 10 to 400.
xr_controller_mouse_sense_y = 100
# Horizontal sensitivity of the head-aimed mouse as a percentage.
# 100 means raw motion. Scales relative movement only, so screen
# edges stay reachable. Valid range from 10 to 400.
xr_head_mouse_sense_x = 100
# Vertical sensitivity of the head-aimed mouse as a percentage.
# 100 means raw motion. Valid range from 10 to 400.
xr_head_mouse_sense_y = 100
# Minimum emulated mouse press length in milliseconds. Releases
# arriving sooner are held until the time is up, so quick trigger
# taps survive slow guest polling instead of vanishing. Lower is
# snappier, higher is safer. 0 releases immediately.
# Valid range from 0 to 200.
xr_mouse_min_hold_ms = 25

# Pointer / input
# xr_ray: controller rays, drawn over all overlays
xr_ray = true
# xr_trigger_threshold: % pull = click, 0-100 (keep above 0 on WMR)
xr_trigger_threshold = 50
# xr_ui_trigger_threshold: % pull for menu/keyboard, 10-100
xr_ui_trigger_threshold = 70

# VFX-1 VIP card emulation
xr_vip_respond = true
xr_log_vip = true
# IRQ line the emulated VFX-1 VIP card raises on. Must match the IRQ
# in the guest VIPPORT setting. The card raises it when command
# traffic completes; the guest EOIs lower it again. Only used while
# xr_vip_respond is on. Valid range from 0 to 15.
xr_vip_irq = 7
# Log every VFX-1 VIP port access with CS:IP and no dedup.
# Off by default. Identical repeats still flip emulated device state,
# so use this when the failure-point log contradicts itself.
# Very verbose: read dosbox-xr.log, not the window.
xr_log_vip_full = true

# Logging
xr_log_lpt = true
xr_log_com = true
xr_log_framestats = false

[osd]
# Desktop-window toast overlay on/off, independent from the headset.
osd_desk = true
# osd_desk_opacity: %, 0-100 (0 hides desktop toasts entirely)
osd_desk_opacity = 50
# osd_desk_scale: %, 25-200
osd_desk_scale = 60
# Size of the desktop mirror-window menu (Ctrl+H) as a percentage
# of its default size ('100' by default). Valid range from 25 to 200.
osd_desk_menu_scale = 100
# Font size of the desktop mirror-window menu (Ctrl+H) text as a
# percentage ('100' by default). Valid range from 50 to 200.
osd_desk_menu_font_size = 100
# Desktop mirror-window menu (Ctrl+H, never shown in the headset)
# osd_desk_menu_scale: %, 25-200
osd_desk_menu_scale = 100
# osd_desk_menu_font_size: %, 50-200
osd_desk_menu_font_size = 100
# In-headset notification overlay on/off, independent from desktop.
osd_xr = true
# osd_xr_distance: cm, 50-500
osd_xr_distance = 190
# osd_xr_scale: %, 25-200
osd_xr_scale = 60
# osd_xr_offset_x: %, -100..100
osd_xr_offset_x = 0
# osd_xr_offset_y: %, -100..100
osd_xr_offset_y = -70
# osd_xr_opacity: %, 10-100
osd_xr_opacity = 70

[su2000]
formatc     = true
formatc_mem = cc000
formatc_io  = 210
su2000_log  = true

[parallel]
#parallel-3 = disabled,dummy - intended for debugging
parallel1 = dummy

```
I'll rewrite and properly add the changes to the default readme in due time.
---
**Original readme below**
---
**DOSBox Staging** is a modern continuation of DOSBox with advanced features and cu rrent development practices.

It is a (mostly) drop-in replacement for older DOSBox versions—your existing configurations will continue to work, and you will have access to many advanced features.

For a detailed description of the project's scope please refer to the [About](https://www.dosbox-staging.org/about/) page on our website.

## Donations

If you enjoy using DOSBox Staging, please [consider a
donation](https://www.dosbox-staging.org/get-involved/#make-a-donation) to the
project.

If you want to help but can't afford a donation, check out the [Get
involved](https://www.dosbox-staging.org/get-involved/) page of our website
for other ways to contribute.


## Project website

https://www.dosbox-staging.org/

First-time users and people migrating from other DOSBox variants should start by reading the [Getting started guide](https://www.dosbox-staging.org/getting-started/).

## Build status

[![Windows build status][build-windows-badge]][build-windows-ci]
[![macOS build status][build-macos-badge]][build-macos-ci]
[![Linux build status][build-linux-badge]][build-linux-ci]

## Stable release builds

Regular users should use our stable release builds available on our project website:

- [Windows](https://www.dosbox-staging.org/download/windows/)
- [macOS](https://www.dosbox-staging.org/download/macos/)
- [Linux](https://www.dosbox-staging.org/download/linux/)


## Development builds

Development builds are automatically created on every commit merged to the `main` branch. You need to be logged in to GitHub to download the development builds.

[Development builds]

## Key features for developers

| **Feature**                | **Status**                          |
| -------------------------- | -------------------------------     |
| **Version control**        | Git                                 |
| **Language**               | C++23                               |
| **Logging**                | Loguru for C++<sup>[3]</sup>        |
| **Build system**           | CMake + Ninja or Visual Studio 2022 |
| **Dependency manager**     | vcpkg                               |
| **CI**                     | Yes                                 |
| **Static analysis**        | Yes<sup>[1],[2]</sup>               |
| **Dynamic analysis**       | Yes                                 |
| **clang-format**           | Yes                                 |
| **[Development builds]**   | Yes                                 |
| **Unit tests**             | Yes<sup>[4]</sup>                   |

[1]: https://github.com/dosbox-staging/dosbox-staging/actions/workflows/clang-analysis.yml
[2]: https://github.com/dosbox-staging/dosbox-staging/actions/workflows/pvs-studio.yml
[3]: https://github.com/emilk/loguru
[4]: https://github.com/dosbox-staging/dosbox-staging/tree/main/tests
[Development builds]: https://www.dosbox-staging.org/download/development-builds/

## Source code analysis tools

- [PVS-Studio](https://pvs-studio.com/pvs-studio/?utm_source=website&utm_medium=github&utm_campaign=open_source) — C++ static analyser
- [Pylint](https://pypi.org/project/pylint/) — Python static analyser
- [markdownlint](https://github.com/DavidAnson/markdownlint) — style checker and linter for Markdown
- [ShellCheck](https://www.shellcheck.net/) — shell script analysis tool

## Dependencies

DOSBox Staging has the following library dependencies:

| Dependency                                                | Provides feature                                | vcpkg package name | vcpkg version     | Optional?           |
| --------------------------------------------------------- | ----------------------------------------------- | ------------------ | ----------------- | ------------------- |
| [Asio](https://think-async.com/Asio/)                     | Network API for emulated serial and IPX         | asio               | 1.32.0            | **no** :red_circle: |
| [FluidSynth](https://www.fluidsynth.org/)                 | General MIDI playback                           | fluidsynth         | 2.5.7             | **no** :red_circle: |
| [GoogleTest](https://github.com/google/googletest)        | Unit testing (development)                      | gtest              | 1.18.0            | yes :green_circle:  |
| [IIR](https://github.com/berndporr/iir1)                  | Audio filtering                                 | iir1               | 1.10.0            | **no** :red_circle: |
| [libmt32emu](https://github.com/munt/munt)                | Roland MT-32 and CM-32L emulation               | libmt32emu         | 2.8.3             | yes :green_circle:  |
| [libpng](http://www.libpng.org/pub/png/libpng.html)       | PNG encoding of screen captures                 | libpng             | 1.6.58            | **no** :red_circle: |
| [libslirp](https://gitlab.freedesktop.org/slirp/libslirp) | General purpose TCP-IP emulator                 | libslirp           | 4.9.1             | yes :green_circle:  |
| [Opus File](https://opus-codec.org/)                      | CD Audio playback for Opus-encoded audio tracks | opusfile           | 0.12+20221121#1   | **no** :red_circle: |
| [SDL 3](https://github.com/libsdl-org/SDL)                | OS-agnostic API for video, audio, and eventing  | sdl3               | 3.4.16#1          | **no** :red_circle: |
| [SDL_image 3](https://github.com/libsdl-org/SDL_image)    | Image decoding for many popular formats         | sdl3-image         | 3.4.4#1           | **no** :red_circle: |
| [SpeexDSP](https://github.com/xiph/speexdsp)              | Audio resampling                                | speexdsp           | 1.2.1#1           | **no** :red_circle: |
| [zlib-ng](https://github.com/zlib-ng/zlib-ng)             | ZMBV video capture                              | zlib-ng            | 2.3.3             | yes¹ :green_circle: |

_¹ You can use plain old zlib instead._


### Dynamically loaded dependencies

**Slirp** is built separately as a dynamic library (via
vcpkg) and is loaded on-demand at runtime if available. Please refer to the
[dosbox-staging-ext](https://github.com/dosbox-staging/dosbox-staging-ext)
project for further details. To check the version of this dependency 
included in our stable and dev build packages, check out the comments of the
[releases packages](https://github.com/dosbox-staging/dosbox-staging-ext/releases) of
the [dosbox-staging-ext](https://github.com/dosbox-staging/dosbox-staging-ext)
project.

For information on the optional **Nuked SC-55 CLAP** audio plugin, check out
the [plugin's GitHub repository](https://github.com/johnnovak/Nuked-SC55-CLAP/) (note Nuked SC-55
CLAP is _not_ a DOSBox Staging project).


## Build instructions

Please refer to the platform specific build instructions:

- [Windows](docs/build-windows.md)
- [macOS](docs/build-macos.md)
- [Linux](docs/build-linux.md)

For building versions prior to 0.83.0 (e.g. when bisecting), see the
[legacy Meson build guide](docs/build-meson.md).


## Website & documentation

Please refer to the [documentation guide](DOCUMENTATION.md) before making
changes to the website or the documentation.


## Imported branches, community patches, old forks

Upstream commits are imported to this repo in a timely manner,
see branch [`svn/trunk`].

- [`svn/*`] - branches from SVN
- [`forks/*`] - code for various abandoned DOSBox forks
- [`vogons/*`] - community patches posted on the Vogons forum

Git tags matching pattern `svn/*` are pointing to the commits referenced by SVN
"tag" paths at the time of creation.

Additionally, we attach some optional metadata to the commits in the form of
[Git notes][git-notes]. To fetch them, run:

``` shell
git fetch origin "refs/notes/*:refs/notes/*"
```

[`svn/*`]:     https://github.com/dosbox-staging/dosbox-staging/branches/all?utf8=%E2%9C%93&query=svn%2F
[`svn/trunk`]: https://github.com/dosbox-staging/dosbox-staging/tree/svn/trunk
[`vogons/*`]:  https://github.com/dosbox-staging/dosbox-staging/branches/all?utf8=%E2%9C%93&query=vogons%2F
[`forks/*`]:   https://github.com/dosbox-staging/dosbox-staging/branches/all?utf8=%E2%9C%93&query=forks%2F
[git-notes]:   https://git-scm.com/docs/git-notes

[gpl-badge]:     https://img.shields.io/badge/license-GPL--2.0--or--later-blue
[discord-badge]: https://img.shields.io/discord/514567252864008206?color=%237289da&logo=discord&logoColor=white&label=discord
[discord]:       https://discord.gg/WwAg3Xf

[build-windows-badge]: https://img.shields.io/github/actions/workflow/status/dosbox-staging/dosbox-staging/windows.yml?label=Windows%20%28x86_64%29
[build-windows-ci]:    https://github.com/dosbox-staging/dosbox-staging/actions/workflows/windows.yml?query=branch%3Amain

[build-macos-badge]: https://img.shields.io/github/actions/workflow/status/dosbox-staging/dosbox-staging/macos.yml?label=macOS%20%28x86_64%2C%20arm64%29
[build-macos-ci]:    https://github.com/dosbox-staging/dosbox-staging/actions/workflows/macos.yml?query=branch%3Amain

[build-linux-badge]: https://img.shields.io/github/actions/workflow/status/dosbox-staging/dosbox-staging/linux.yml?label=Linux%20%28x86_64%29
[build-linux-ci]:    https://github.com/dosbox-staging/dosbox-staging/actions/workflows/linux.yml?query=branch%3Amain
