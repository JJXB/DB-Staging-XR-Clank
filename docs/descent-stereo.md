# Descent 1 (descentr.exe v1.5) — Stereo Glasses Modes Infodump

Binary: `D:\Emu\DOS\C\descent\descentr.exe` (1,132,633 bytes, `-Registered
v1.5 Jan 5, 1996`, Watcom C 32-bit + DOS4GW LE executable).
Reference: official Descent 1 v1.5 public source
(github.com/videogamepreservation/descent, `MAIN/` uppercase names).
Every `$Id` below was verified present in BOTH the exe (raw-string scan)
and the public source at the same revision — the public source IS this
binary's source, except `vfxread.c` (ships in the exe, removed from the
public tree).

## 1. Seeing the LE payload (method note)

`rz-bin` opens the file with the `mz` plugin (DOS stub) and sees no game
strings. The DOS4GW stub's `e_lfanew` is garbage (`0x09B40000`, past EOF),
so carving needs a signature scan: exactly one `LE`-magic candidate
validates (file offset `0x37B5C`, fmtlvl 0, 174 pages, 2 objects).
Carve with `LE = bytes[0x37B5C..EOF]` (`descentr_le.bin`, 904,445 bytes);
from there the rizin `le` plugin works: 32-bit x86, code object
(vaddr `0x10000`, RX), data object (vaddr `0xB0000`, RW), entry
`0x73350` (valid Watcom startup code with relocations applied).
Raw-string scan (CP1252 printable runs ≥5) finds all 3,914 strings with
raw file offsets; carved offset = raw − `0x37B5C`. The plugin's own
`-z` string list is partial (561 strings) — trust the raw scan.

## 2. Launch surface (exe-verified)

`CM.BAT` = `descentr -cybermaxx`; `CMF.BAT` = `serial2=cybermaxx` (dosbox-xr
conf note) + `descentr -cybermaxx -maxxtrack 2`; `IG.BAT` =
`descentr -iglasses`; `IGF.BAT` = `serial2=iglasses` +
`descentr -iglasses -viotrack 2`; `VFX.BAT` = `descentr -vfx -vfxtrack`.
`descent.cfg` carries `VR_type=0` + `VR_tracking=0` (vanilla: both 0).

Arg help block (raw `0xFDA62…`, overlapping printf fragments):
`-simuleyes` (StereoGraphics SimulEyes), `-iglasses`, `-viotrack <n>`,
`-3dmaxlo`, `-3dmaxhi` (Kasan 3DBIOS, `INT 10h AX=4FD0h`), `-lcdbios`
(LCDBIOS shutter, warn + Esc-bypass), `-cybermaxx`, `-cybermax2`,
`-maxxtrack`, `-vfxtrack`, `-vfx`, `-320x200/-320x240/-320x400/-640x400/
-640x480/-320x100`, `-nodoublebuffer`, plus `Invalid serial port
parameter for -maxxtrak!`, `Couldn't find VIPPORT environment variable.`,
`CYBERMAXX` env with `x4/E:R` fragment, `Stereo parameters reset`,
`Head tracking sensitivy = %d`, `Stereoscopic balance`, `Stereo
separation`, `StereoReverse`, `VR_tracking`, `Victor mode toggled`.

`INFERNO.C:1380…1541` (arg + `Config_vr_type` handling):

| Flag / Config_vr_type | Device | Screen mode | Render (w×h) | Paging | Render method | VR_low_res |
|---|---|---|---|---|---|---|
| `-vfx` / type 1 | VFX-1 (+`-vfxtrack` sense) | SM_640x480V (14) | 320×240 | 0 | VR_AREA_DET (1) | 3 |
| `-cybermaxx` / type 2 | CyberMaxx (+`-maxxtrack N`) | SM_320x200C (0) | 320×100 | 0 | VR_INTERLACED (2) | 0 |
| `-iglasses` / type 3 | i-glasses! (+`-viotrack N`) | SM_320x400U (6) | 320×200 | 1 | VR_INTERLACED (2) | 3 |
| `-cybermax2` / type 6 | CyberMaxx-2, video only (no tracker init!) | SM_320x400U (6) | 320×200 | 1 | VR_INTERLACED (2) | 3 |
| `-3dmaxlo/hi` / types 4,5 | Kasan 3DBIOS (must be installed) | 320x200C / mode 22 | 320×100/200 | 0/1 | VR_INTERLACED | 0 |
| `-lcdbios` / `-simuleyes` | LCD shutter / SimulEyes | 320x200C (or 320x400 ModeX) | … | … | VR_INTERLACED / flip | … |

Render methods (`GAME.H:378…380`): `VR_NONE 0`, `VR_AREA_DET 1`,
`VR_INTERLACED 2`. Eye globals (`GAME.C:414…432`): `VR_use_paging`,
`VR_current_page`, `VR_switch_eyes`, `VR_eye_width` (def.
`VR_SEPARATION`), `VR_eye_offset` (def. `VR_PIXEL_SHIFT -6`),
`VR_render_buffer[2]` (per-eye offscreens), `VR_screen_pages[2]`.
`CYBERMAXX=/E:R` in the environment sets `VR_switch_eyes = 1`
(`INFERNO.C:1530…1541`; exe fragment `x4/E:R` at raw `0xFE06A`).
Alt+V / Shift+F5 toggle Victor interleaved↔normal at runtime
(`GAME.C:3584…3589`, `T-%Victor mode toggled` at raw `0xFE3C5`).
Shift+F5 otherwise resets stereo params; Shift+Z recenters, but
`kconfig_center_headset()` bodies are commented out in this rev
(`KCONFIG.C:2215…2220` — centering is a no-op).

## 3. Video paths (the decode reference)

Both serial headsets render in `game_render_frame_stereo_interlaced`
(`GAME.C:1649…1851`): left eye → `RenderCanvas[0]`, right eye →
`RenderCanvas[1]` (`render_frame(∓VR_eye_width)`; `VR_switch_eyes`
swaps; rear view swaps). HUD is drawn into BOTH eye canvases with a
`VR_eye_offset` horizontal shift (`GAME.C:1693…1702,1725…1734`) — HUD
combing in a raw capture is EXPECTED, not corruption.

Bitblt mechanics (`2D/BITBLT.C:119…120,230,352…`): arg order
`(w,h,dx,dy,sx,sy,src,dest)`; `gr_bitblt_dest_step_shift` left-shifts
the dest row stride (1 = stride ×2, "skip every other scanline";
2 = stride ×4); `gr_bitblt_double` doubles horizontally.

- CyberMaxx (`-cybermaxx`): `VR_low_res = 0`, so the plain branch
  (`GAME.C:1810…1823`, step_shift 1): dest rows 0,2,4… ← left rows,
  dest rows 1,3,5… ← right rows into the 320x200 screen. Framebuffer
  pattern = **LRLR, even = left** (modulo `VR_switch_eyes`), each eye
  320x100. `victor_init_graphics()` (`VICTOR.C:185…194`, Alt+V toggled)
  swaps two hard-coded VGA register sets (`VGA13` normal 320x200 vs
  `VGAi13` interleaved twin — MOR `0x27` vs `0x63`, full SR/CRTC tables
  in source) to make the CRT scan the buffer as fields for the panels.
- i-glasses! / cybermax2 (`-iglasses`, `-cybermax2`): `VR_low_res = 3`
  (bits 0+1: eyes rendered at half size, `sw = 160`, `sh = 100`,
  aspect hacked ×2÷2), horizontal double on, step_shift 2
  (`GAME.C:1790…1809`): four blits at dest-y 0,1,2,3 with stride 4 —
  dest rows 0,4,8… ← left, 1,5,9… ← right, 2,6,10… ← left (repeat),
  3,7,11… ← right (repeat) into the 320x400 ModeX screen. Framebuffer
  pattern = **LLRR row pairs** (`L0,R0,L0,R0,L1,R1,…`), i.e. LRLR with
  every row doubled; each eye effectively 320x200. No VGA reprogramming
  (standard `SM_320x400U`); the headset's scanline router does the rest.
- VFX-1 (`-vfx`): `game_render_frame_stereo_vfx` (`GAME.C:1580…1644`)
  renders both eyes then blits them SIDE BY SIDE into
  `VR_screen_pages[0]` and flips the VIP page-select register
  (`vfx_set_page`: page 0 → left 0/right 1, else 2/3; `VFX.C`
  `PORT_AIR`/`PORT_CSR` = base+0/+1, `INDEX_PSR`). Palette is snooped
  into the VIP (`vfx_set_palette_sub`, 768 bytes via index 10), stereo
  bit at base+3 (`vfx_init_graphics`, overscan-derived). Video never
  touches the VGA framebuffer as stereo — decode accordingly (page flip,
  not row weave). `vfx_init` reads base from `VIPPORT` env (default
  `0x260`) and ERRORS without it — matches our `VFX1.md`.

Emulator decode mapping (what dosbox-xr implements in
`gui/{cybermaxx,iglasses}.cpp`, `xrstereo.cpp`):

| Game mode | Screen buffer | Row pattern (default) | Decode |
|---|---|---|---|
| `-cybermaxx` 320x200 | 320×200 | even L / odd R | half-height fields, GPU upscale |
| `-iglasses`/`-cybermax2` 320x400 | 320×400 | L,R,L,R,… (every row ×2) | same rule — the doubled rows are harmless duplication |
| Render scaler ×2/×3 of either | H×2/H×3 | L,L,R,R,… pairs | per-frame pair detector → de-double first |

Decode source is the PRE-SCALER game frame (`last_framebuf` at
`input_texture` dims via `ReadGameFrame`), never the window-fitted
backbuffer: fractional presentation scale, CRT-shader row filtering,
and bottom-up `glReadPixels` order all destroy or displace the fields
there (even screen rows land on odd vector rows for even heights, i.e.
a clean left↔right swap on top of the combing). Integer render-scalers
(none/2x/3x) keep fields exact, which is all the row rule needs.

## 4. Tracker protocols (serial-device reference)

- CyberMaxx (`VICTOR.C:196…229`, `-maxxtrack 1…4`): 19200 8N1, DTR+RTS
  on, no RTS/CTS. Init single chars: `A` (absolute), `C` (high
  accuracy), `F` (**binary mode**), `G` (sample on request), then `S`
  per packet. Packets are **6-byte big-endian shorts** (NOT the ASCII
  `Y/P/R` lines — that mode is never selected here):
  `yaw = s16`, `pitch = -(p − 16384)`, `roll = -(r − 16384)`
  (`VICTOR.C:267…275`; 65536 units = 360°, so yaw is ±180° raw and
  pitch/roll ±90° around the 16384 bias). Short reads hold last values
  and re-request (`count < 6` path); final output averages 2 readings
  (`victor_read_headset_filtered`). Emulator serves this natively:
  dosbox-xr `serial cybermaxx` streams ASCII `Y/P/R` until the first
  A/C/F/G/S byte, then answers each `S` with a 6-byte big-endian packet
  (DTR drop restores the ASCII stream).
- i-glasses! (`IGLASSES.C:111…176`, `-viotrack 1…4`): 9600 8N1,
  DTR+RTS on, no RTS/CTS. `!\r` until `O` (1 s windows, Esc aborts),
  `!M1,P,B\r` until `O` (cooked/polled/binary — matches
  `IGLASSES.md` §§3–5 and our `gui/trackers` core), then `S` loop.
  12-byte cooked packets, additive checksum over bytes 0…10
  (`IGLASSES.C:257…265`); host runs mag de-rotation + 6-tap FIR filters
  and re-baselines yaw/pitch/roll over the first 30 readings
  (`BasisYaw/Pitch/Bank`, `IGLASSES.C:294…300`) — a static pose at
  startup reads as CENTERED, and early motion is swallowed by the
  baseline. (An 8-byte Euler `!M2` reader exists but is `#ifdef`'d out
  as `iglasses_read_headset1`.)
- Control integration (`KCONFIG.C:1402…1405`): per-frame
  `iglasses_read_headset` / `victor_read_headset_filtered` feed
  yaw/pitch/roll into flight controls; `VR_sensitivity` 0–2 scales.

## 5. Claim → source map

- Exe identity/help/args/BATs/cfg: `D:\Emu\DOS\C\descent\` (`CM.BAT`,
  `CMF.BAT`, `IG.BAT`, `IGF.BAT`, `VFX.BAT`, `descent.cfg`,
  `descentr.exe` raw `0xFDA1F` version, `0xFDA62…` help,
  `0xFF320 VIPPORT`, `0xFF6C8 Invalid serial port parameter…`).
- Setup table: `MAIN/INFERNO.C:1380…1541` (tracker init 1386…1396,
  video table 1398…1431, `CYBERMAXX` env 1530…1541).
- Render core: `MAIN/GAME.H:378…380` (methods), `MAIN/GAME.C:955…992`
  (buffers: top/bottom halves for INTERLACED), `1649…1851`
  (interlaced renderer; weave blits 1789…1824), `1580…1644` (VFX
  renderer + `vfx_set_page`), hotkeys 3579…3604.
- Bitblt proof: `2D/BITBLT.C:119…120` (globals), `:230` (stride
  shift), `:352…` (`gr_bm_ubitblt01` arg order `(w,h,dx,dy,sx,sy,…)`).
- Modes: `SM_*` in `2D` headers (`SM_320x200C 0`, `SM_320x400U 6`,
  `SM_640x480V 14`).
- Trackers: `MAIN/VICTOR.C:196…300` (19200/ACFG/6B/binary/scales),
  `MAIN/IGLASSES.C:111…310` (9600/`!`/`!M1,P,B`/12B/FIR/baseline),
  `MAIN/VFX.C` (VIPPORT/PSR/palette/stereo-bit), `MAIN/KCONFIG.C:1402…
  1405,2215…2220` (control feed, no-op centering).
- Vendor docs this cross-checks: `VMaxxCyMaxx.md` §6 (ASCII mode =
  the mode Descent does NOT use), `IGLASSES.md` §§3–6 (all confirmed:
  `!M1,P,B`, 12B layout + checksum, +16384 = 180°, `rawToEulerInt`
  equivalents at `IGLASSES.C:279…290`).
