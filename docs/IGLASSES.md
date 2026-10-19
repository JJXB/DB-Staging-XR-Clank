# Virtual i-O i-glasses! — Tracker + Video Infodump

Source trees surveyed:

- VR Slingshot game `D:\Emu\DOS\C\CY\VRSLING` (VRS.EXE 15/10/1995,
  `MANUAL/README.TXT:345-376`, `VRS.CFG`/`VRSDEF.CFG`, full disassembly in
  `VRSLING.md`). Game-side consumer: `TRACKER_TYPE = I_GLASSES` on a COM
  port, `320x480/Interleaved` video, F12 software forward, keypad gamma.
- Vendor SDK `D:\Emu\DOS\C\VRDRV\viosdk` (**all** C sources + headers read:
  `SRC/TRACKER1/` TRACKER/TRW/TCAL/SPACERAT/RAT/VTMODE/TEST/PTEST,
  `SRC/SERIAL1/` SERIAL/SERBIH, `SRC/CONVERT1/`, `SRC/IMATH1/`,
  `SRC/GRID1/` GRID/GRIDEMO/TEST, `SRC/VGFX1/`, `SRC/DRAMGFX1/`,
  `SRC/CLIP1/`, `SRC/TIMER1/`, `APPS/SIMPLE/` TEST/FTEST,
  `APPS/BSIMPLE/` TEST + `VMOUSE/VMOUSE.C`, `APPS/TCAL/TCAL.C`,
  `APPS/TRACKMAN/TRACKER.C`, `INCLUDE/` SERIAL1/TRACKER1/VSTRACK1,
  `DEVKIT1.TXT`). No binaries were disassembled — the SDK ships source.
- Vendor docs: `iglassesSDK.pdf` (Developer Kit v1.2, 6/95: §§A–E) and
  `viosdk/MANUAL.DOC` (same text, confirmed Mode 4 line only).

Sibling files: `VMaxxCyMaxx.md` §12 (CyberMaxx game path, same VRS
machinery), `cybershades.md` §11 (LCD-shutter path), `VRSLING.md`
(game-side index).

## 1. The 30-second version

1. VRS treats i-glasses! as a **serial head tracker on a COM port** —
   menu `TRACKER_TYPE = I_GLASSES` (`CB_STR_TT_I_GLASSES`,
   `COMBO_TRACKER_TYPE`), Tracker Port = COM port
   (`COMBO_CYBERMAXX_PORT` family, `CB_STR_MP_COM1-4`). Same port-select
   machinery as the CyberMaxx tracker option; only the enum differs.
2. The packet grammar is **fully known from the vendor SDK** (this file
   previously said "not reversed" — superseded): VRS sends `!M1,P,B\r`
   (cooked, polled, binary; string at VRS.EXE file `0xEBDD0` next to
   `Yaw %4.2f / Pitch %4.2f / Roll %4.2f`), then loops `S` request → 12-byte
   cooked packet → host-side yaw/pitch/roll conversion. No ISA probe, no
   IRQ, no TSR.
3. Wire protocol is RS-232 3-wire (TXD/RXD/GND), 1200/2400/4800/9600/19200
   bps, 8N1 (`iglassesSDK.pdf` §B; `SRC/SERIAL1/SERIAL.C` `setBPS`,
   `initTracker` asserts DTR+RTS, disables CTS check). All commands except
   `S` are printable ASCII `!`…`\r` (0x0D) with `O`/`E` replies; `S`
   requests (`S` = send polled packet, or start stream in continuous mode;
   `!`+`\r` stops the stream).
4. Video is **scanline-interleaved** (`iglassesSDK.pdf` §§D–E,
   `SRC/GRID1/GRIDEMO.C`, `SRC/DRAMGFX1/DRAMGFX.C`): one 320x400 frame
   carries two 320x200 eye views on alternate scanlines, first scanline =
   left eye. Same family as the CyberMaxx panel interleave
   (`VMaxxCyMaxx.md` §7); VRS's `IL320X480`/`s`-toggle path matches.
5. View layer is game-software only: F12 sets a new forward direction
   and explicitly does **not** recalibrate the headtracker; keypad
   `*`/`-`/`+` gamma brightness; save writes `VRS.CFG`.

## 2. VRS consumer (game side, unchanged)

File/Hardware Setup → Tracker Port (COM the device is plugged into) +
Tracker Type (i-glasses! or CyberMaxx); `320x480/Interleaved`, `s`
toggles 3D; F12 forward view (software only); keypad gamma; Save
(`MANUAL/README.TXT:345-376`).

Port layer: init at file `0xA9990` derives `base+0xA → [0x27450]`,
`base+0xC → [0x27452]`, `base+0xE → [0x27454]/[0x27456]`,
`base+0xF → [0x27458]`; poll at `0xA95F0-0xA9689` is status-gated
serial RX (`IN [0x27454], TEST 0x80` then `IN [0x27450]`, mirrored
`TEST 0x80 / OUT` on `[0x27452]`, `CMP [0x2745A],0x400` + `0x82`
divisor-style gate). For the shared COM base-table/divisor/status-bit
machinery see `VRSLING.md` §§7–8; the tracker bytes on that port are the
§3–§4 packets below.

## 3. Tracker command set (vendor SDK)

From `SRC/TRACKER1/TRACKER.C` (`resetTracker`, `sendTrackerCMD`,
`getTrackerResponse`, `getTrackerVerStr`, `requestTrackerData`) and
`iglassesSDK.pdf` §§B-2–B-4:

- `!\r` — reset handshake. `resetTracker()` flushes, then sends `!\r`
  in a loop (1-tenth-sec inner window, caller timeout outer) until a
  bare `O` arrives. Recovery rule (`DEVKIT1.TXT` Notes): if the tracker
  stops answering, send `!\r` until `O` — re-sending `S` alone may never
  recover. `APPS/TRACKMAN/TRACKER.C` `resetTracker3bang` uses `!!!\r`
  with the comment "will be `!\r` when firmware bugs fixed".
- `!R\r` — reset to default state: cooked, polled, binary. All apps
  then select a real mode before reading.
- `!V\r` — 60-byte version string (`TRACKER_VER_LEN 60`,
  `getTrackerVerStr` null-terminates over the trailing `O`/`E`, rejects
  when `[0] != 'M'`): format
  `M<16 chars>P<16 chars>T<8 chars>Hxxx.xxxFxxx.xxx` + `O` (self-test
  pass) / `E` (fail). `getTrackerVerRev` parses hw/fw numbers at
  offsets 44/48/52/56 (`HVER/HREV/FVER/FREV_OFFSET`). `SRC/TRACKER1/PTEST.C`
  prints it; `APPS/TRACKMAN` shows `Hardware x.xxx Firmware x.xxx`.
- `!M<data>,<send>,<format>[,<magfilt>,<grvfilt>][,<xs>,<ys>,<xt>,<yt>]\r`
  — set data mode 0–4, send mode (`P` polled / `C` continuous; `0`–`1`
  mouse delta/joystick in mouse mode), format (`A` ASCII hex debug /
  `B` binary), optional filters 0–7 (0 none, 7 max; app should leave to
  Tracker Manager) and mouse sensitivity/threshold 0–9 each.
  `SRC/TRACKER1/VTMODE.C` builds exactly this string (`!M%d,%c,%c…`).
- `S` (no `\r`) — send one packet in polled mode; start the stream in
  continuous mode. `requestTrackerData()` writes a single `'S'`.
- `!` + `\r` — stop a continuous stream so the pending command parses.
- `!ZR,aaaa\r` / `!ZW,aaaa,vv\r` — tracker-memory byte read/write
  (`SRC/TRACKER1/TRW.C`; read returns value byte + `O`). `DEVKIT1.TXT`
  warns apps **not** to use these (subject to change, undocumented);
  use `!M` filter/mode commands instead.
- `!MC,M,B\r` + `S`, `!MC,G,B\r` + `S…` — factory magnetic/gravimetric
  calibration sessions (`APPS/TCAL/TCAL.C` `doMagneticCal`,
  `doGravimetricCal`; mag stream is 1 + 19 bytes, fields minus
  `T_OFFSET 2048`). `DEVKIT1.TXT`: do **not** calibrate by hand —
  factory jigs required.
- Timeouts: `TIMER1` `timerSecs/timerTSecs/timerHSecs` (18.2 Hz or
  `clock()`); canonical `!M…` waits 2–3 s, packet reads 1 s, reset
  1 tenth-sec units. Every sample app follows
  `request → read → request → …`, flushing (`flushSer`) on
  timeout/checksum failure.

## 4. Tracker packet formats (vendor SDK)

Orientation convention (`iglassesSDK.pdf` §B-1, `SRC/CONVERT1/CONVERT.C`
header): from the wearer's perspective **+yaw = left turn, +pitch = up
tilt, +roll = left tilt**; right-handed +Y up / +Z out / +X right.
Euler packet scale: signed 16-bit, **+16384 = 180°**
(`degrees = reading/16384*180`; host `TOFLOAT` = `/16384` after the
`×180` fixed-point step — see `readTrackerEuler`).

- **Mode 0 raw, 12 bytes** (`iglassesSDK.pdf` §B-4, `readPacket(td,buff,12)`):
  `[255][XH][XL][YH][YL][ZH][ZL][PH][PL][RH][RL][checksum]`,
  checksum = arithmetic sum of bytes 0–10. Sensors raw: 12-bit unsigned
  0..4095 packed in 16-bit words. Factory-debug only.
- **Mode 1 cooked, 12 bytes** (same layout; `readTracker()`):
  magnetometer X/Y/Z ≈ ±16384 (varies with local Earth field),
  tilt P/R linearized so +16384 = 180°. Host converts with
  `rawToEulerInt()` (§6). **This is the VRS mode** (`!M1,P,B`).
- **Mode 2 Euler (micro-Euler), 8 bytes** (`readTrackerEuler()`):
  `[255][YawH][YawL][PitchH][PitchL][RollH][RollL][checksum]`,
  checksum = sum of bytes 0–6. Firmware already ran the angle math;
  host stores `euler.x = p*180, .y = y*180, .z = r*180` (14-bit fixed).
  `GRID_MODE_MICRO_EULER` / `APPS/TCAL` `M` key / `FTEST -e` use it.
- **ASCII format** (`A`): same bytes rendered as hex with spaces —
  debugging only, never for shipping apps.
- **Mode 3 mouse emulation, 1200 bps** (`iglassesSDK.pdf` §§B-8–C,
  `SRC/TRACKER1/RAT.C`, `SPACERAT.C`, `APPS/TRACKMAN/TRACKER.C`):
  Microsoft 3-byte packets (header bit 6 set; `lox/hix/loy/hiy`
  reassembly in `RAT.C`/`spacerat()`), X = scaled yaw, Y = scaled
  pitch. Send mode 0 = deltas while moving; 1 = joystick-style deltas
  from a reference until back inside threshold. Sensitivity 0–9
  (sens 1: 1 X-mickey = 1/4° yaw, 1 Y-mickey = 1° pitch; 0 disables the
  axis — DOOM preset disables Y), threshold 0–9 (low = small packets
  often, high = large packets rarely). Manager defaults: filters 3/3,
  sens X 1 / Y 3 (help text; sliders default X 1 / Y 4), threshold 2/2.
  Entering true mouse mode drops the port to 1200 bps (`setBPS`),
  re-syncs with `!!!\r`, then streams on `S`. The tracker **remembers**
  its last mode across power-off — Tracker Manager exists so
  mouse-games and tracker-games can share the device.
- **Mode 4 CyberMaxx emulation — [Not yet implemented]**
  (`iglassesSDK.pdf` §B-9, `MANUAL.DOC` line 118). The SDK reserves the
  slot but ships no code; do not expect `!M4…` to work on 1.x firmware.
  (VRS's separate `CYBERMAXX` tracker type is the VictorMaxx ASCII
  `Y/P/R` protocol in `VMaxxCyMaxx.md` §6, not this slot.)

Firmware history (`iglassesSDK.pdf` §B-12): HW 001.000 (2/7/95 initial),
001.001 (4/25/95 A/D); FW 001.000 (2/7/95), 001.001 (2/15/95 filter +
serial), 001.002 (3/13/95 Logitech mouse bug), 001.003 (4/25/95 Euler
yaw sign). Pre-001.003 trackers need the yaw sign flipped
(`FTEST -remap`: `ey = -y + 270`, wrap past 180).

## 5. Host driver shape (what a game does)

Canonical init (`SRC/TRACKER1/TRACKER.C` `initTracker`,
`initTrackerSearch`; every sample app follows it):

1. `calloc` a `serialData`, `initSerial(sd, COM1–4, USE_16550, 1024, 1024)`
   (interrupt-driven bi-modal handler, §7), `setBPS(9600|19200)`,
   `setDTR(1) + setRTS(1)`, `setCheckCTS(0)`.
2. `resetTracker(2 s)` — `!\r` until `O`.
3. `sendTrackerCMD("!M1,P,B\r")` for cooked (VRS, GRID, SIMPLE TEST,
   VMOUSE) or `"!M2,P,B\r"` for Euler (TCAL default, FTEST `-e`).
4. Loop: `requestTrackerData()` (`S`) → `readTracker()` (12 B +
   checksum + `rawToEulerInt`) or `readTrackerEuler()` (8 B +
   checksum) → `requestTrackerData()` → … On timeout/checksum:
   `flushSer`, optionally `resetTracker`, continue.

`TrackerData` (`INCLUDE/TRACKER1.H`, `VSTRACK1.H`): `sd` serial handle;
`magnetic` vec3 (raw/cooked XYZ); `gravimetric` vec2 (P/R);
`euler` vec3 **x = pitch, y = yaw, z = roll** in 14-bit fixed
(`ONE = 1<<14`, `TOFLOAT(n) = n/16384.0`); `magOff/magGain`,
`grvOff/grvGain`, `magFilts/grvFilts`. TCAL memory map
(`SRC/TRACKER1/TCAL.C`): `BASE_ADDR 0x4030`; gains
`+0x30…0x38`, offsets `+0x20…0x28`, filters `MF +0x10 / GF +0x12`;
misc vars `DATA_MODE 0x04 / MOUSE_MODE 0x05 / SEND_MODE 0x06 /
DATA_FORMAT 0x07`, mouse `XS 0x42 / XT 0x43 / YS 0x46 / YT 0x47`.

Real-mode note (`DEVKIT1.TXT` §16-bit, `APPS/BSIMPLE/`): Borland 3.1
builds use `imath/vector/timer/filter/convert/serial/tracker` +
`vstrack1.lib`; `VMOUSE.C` is a Heretic external driver exposing
`Vmouse.angle/pitch` on INT `0x60–0x67` (`yaw>>7` wrap, `pitch>>13`).

## 6. Host angle math (cooked → Euler)

`SRC/CONVERT1/CONVERT.C` `rawToEulerInt()` (integer, no sqrt; worst-case
error 12/1000°, mean 4/1000° vs float — `DEVKIT1.TXT`):

1. FIR-filter mag (3-tap x/y/z) and tilt (6-tap); `linGrav =
   rawGrav × π` (fixed-point `MUL(raw,RAD180DEG)`).
2. `sincos(pitch / roll)` from the 256-entry 28-bit interpolated
   sine/atan tables (`SRC/IMATH1/IMATH.C`).
3. De-rotate the magnetic vector into definition space: pitch about X
   (`roty = cos·y − sin·z`, `rotz = sin·y + cos·z`), then partial roll
   about Z (`rotx = cos·x − sin·roty`).
4. `yaw = TODEGREES(getatan2(rotz, rotx))`, `pitch/roll =
   TODEGREES(linGrav.x/y)`.

`FTEST.C` cross-checks three yaw paths per frame (float atan2 vs
fixed cooked vs micro-Euler) and prints `FY/CY/EY/DY/FCDY`.

## 7. Serial driver (what the COM port looks like)

`SRC/SERIAL1/SERIAL.C` (.H, `SERBIH.ASM` real-mode stub): BIOS base
table at `0x400`, COM1/3 → IRQ4 vector `0x0C` mask bit 4, COM2/4 →
IRQ3 vector `0x0B` mask bit 3; divisor = `115200/bps`; 8N1
(`LC_WORD_LEN_8`); 16550 FIFO probe (`FIFO_INIT`, `0xC0` match →
`FIFO_SET`, else clear); `INT_EN_SET` = TX-empty + RX-ready +
delta-modem; `MCR = OUT2|RTS|DTR`; PIC unmask + dummy IIR/RBR/LSR/MSR
reads; Watcom-386 bi-modal protected+real handlers with DPMI locking
(`sbhInt/sbhEnd`, `lockMemory`), DOS-memory queues (default 1024/1024
in tracker use); `txHandler` honors CTS only when `checkCTS`
(tracker use: off); `setBPS` clamps 300–115200.

## 8. Stereo video (interleaved 320x400)

`iglassesSDK.pdf` §§D–E + `SRC/GRID1/GRIDEMO.C` + `SRC/DRAMGFX1/DRAMGFX.C`
+ `SRC/VGFX1/VGFX.C` + `SRC/CLIP1/CLIP.C`:

- Two 3D paths: **field/frame-sequential** (NTSC tape compatible: even
  scanlines one eye / odd the other; VGA 320x200/320x400/640x200/640x400/
  640x480 page-flipped at 60 Hz, ≥4 pages for double-buffered flip) and
  **interleaved** (one frame buffer, scanline L,R,L,R…; 320x400 → 2×
  320x200, 640x400 → 2× 640x200, 640x480 → 2× 640x240, Enigma 320x200 →
  2× 320x100; **first scanline = left eye**).
- Direct path (VGFX): Mode-X-ish `regs320x400F` table
  (`MISC 0x63`, CRTC/SEQC/GFX/ATTRC), unchain (`CHAIN4_OFF`,
  `DWORD_OFF`, `BWORD_ON`), 4 page addrs `vmpage[]`, per-row
  `yOffset[i] = i*80*2` in stereo vs `i*80` mono (`computeOffsets`),
  `setViewPageAddr(vpage<<15)`, `waitVS` on `0x3DA` bit 3.
- DRAM path: render both eyes into a linear 320x400 buffer
  (`d_drawGrid(dram)` + `(&dram[320*200])`), then
  `d_copyPagesStereo400` weaves them (`copyPage200i` skips every other
  destination scanline `destAddr += 20`).
- Projection: `project3stereo`: `x = center.x − (p.x+msep)·vmag.x/p.z`,
  `y = center.y + p.y·vmag.y/p.z`; GRIDEMO defaults `msep 2729, sep 11,
  zoom .125/16+.0005`, stereo centers `159∓sep`, `vmag.y ×10/11`
  (stereo) vs `×10/11×2` (mono). Keys while running: `s` stereo,
  `7/8` separation, `9/0` inter-ocular, `-`/`=` zoom, `\` reset, `a`
  angles/fps, `b` background, `d` DRAM, `v` vsync, `q/ESC` quit —
  the same key family VRS documents (`VRSLING.md` §6).
- OSD: `Y %6.2f P %6.2f R %6.2f` + `%2.f fps` (`displayAngles`) —
  compare VRS's `Yaw %4.2f / Pitch %4.2f / Roll %4.2f` at `0xEBDD0`.
- Modes: `GRID_MODE_COOKED 0` (`readTracker` + host math) vs
  `GRID_MODE_MICRO_EULER 1` (`readTrackerEuler`); GRID default baud
  19200, SIMPLE default 9600.

## 9. Emulator note

- Snoop/synthesize on the configured COM port: expect `!\r` reset
  polling at init, then `!M1,P,B\r` (VRS/cooked) or `!M2,P,B\r`
  (TCAL/GRID-Euler), then a steady `S` → 12-B (or 8-B) packet cadence
  with header `255` and additive checksum. Reply `O` to `!`-commands
  (plus the 60-B `!V` string when asked); stream packets on each `S`.
  F12 and gamma stay host-side transforms.
- Video: de-interleave the 320x400 (VRS: 320x480 `IL320X480`) frame
  into even→first-eye / odd→second-eye half-height fields (GPU-upscaled
  on upload), first scanline left (honor `xr_eye_swap` on top) — same
  row rule as the CyberMaxx path (`VMaxxCyMaxx.md` §7), only the
  panel/brand differs. A vertically line-doubled capture (VGA/SDL
  scaling duplicating every row) is detected per frame and de-doubled
  first.
- For the shared VRS COM base-table/divisor/status-bit machinery see
  `VRSLING.md` §§7–8; for the sibling CyberMaxx consumer (same
  init/poll code, different enum + VictorMaxx `Y/P/R` ASCII) see
  `VMaxxCyMaxx.md` §12.

## 10. Claim → source map

- Enum/port/video/F12/gamma → VRS.EXE strings `CB_STR_TT_I_GLASSES`,
  `COMBO_TRACKER_TYPE`, `COMBO_CYBERMAXX_PORT`, `CB_STR_MP_COM1-4`,
  `CB_STR_S3D_IL320X480`; manual `README.TXT:345-376`.
- VRS init/poll → file `0xA9990` (derived vars), `0xA95F0-0xA9689`
  (`TEST 0x80` gated RX), `CMP [0x2745A],0x400` + `0x82` gate.
- VRS tracker mode string + OSD → VRS.EXE file `0xEBDD0` (`!M1,P,B\r`
  + `Yaw/Pitch/Roll %4.2f`).
- Wire/commands/packets/orientation/scale → `iglassesSDK.pdf` §§B-1–B-4
  (`!R/!V/!M/S`, 12-B/8-B layouts, checksums, +16384 = 180°, 8N1,
  1200–19200 bps); `viosdk/SRC/TRACKER1/TRACKER.C` (all handlers,
  `TRACKER_VER_LEN`, ver offsets, `readTracker`/`readTrackerEuler`
  field order); `TRACKER.H` / `INCLUDE/VSTRACK1.H` (`TrackerData`,
  euler x=pitch/y=yaw/z=roll, 14-bit fixed).
- Mouse/Mode-4/versions → `iglassesSDK.pdf` §§B-8–B-13;
  `viosdk/SRC/TRACKER1/RAT.C`, `SPACERAT.C`, `VTMODE.C`
  (`!M%d,%c,%c…` builder), `TCAL.C` (mem map `0x4030`, `!MC,…`,
  `T_OFFSET`); `viosdk/MANUAL.DOC:118` (Mode 4 unimplemented);
  `APPS/TRACKMAN/TRACKER.C` (manager UI/defaults, `!!!\r`, 1200-bps
  mouse entry, remember-across-power-off).
- Angle math → `viosdk/SRC/CONVERT1/CONVERT.C`,
  `SRC/IMATH1/IMATH.C`, `DEVKIT1.TXT` (14-bit, tables, error bounds);
  `APPS/SIMPLE/FTEST.C` (triple-yaw check, `-remap` for pre-001.003).
- Serial driver → `viosdk/SRC/SERIAL1/SERIAL.C` (+`.H`, `SERBIH.ASM`).
- Video → `iglassesSDK.pdf` §§D–E; `viosdk/SRC/GRID1/GRIDEMO.C`
  (+`GRID.C`, `TEST.C`, `GRIDEMO.H`), `SRC/VGFX1/VGFX.C`,
  `SRC/DRAMGFX1/DRAMGFX.C`, `SRC/CLIP1/CLIP.C`; `APPS/TCAL/TCAL.C`
  (`G`/`M` grid keys); `APPS/BSIMPLE/VMOUSE/VMOUSE.C` (Heretic driver).
