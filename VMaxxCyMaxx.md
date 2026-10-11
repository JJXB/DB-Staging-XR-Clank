# VictorMaxx CyberMaxx VR Helmet — Infodump

Source pages (Mellott's VR, Kevin Mellott):
- Technical teardown: `https://mellottsvrpage.com/index.php/cybermaxx-virtual-reality-helmet/`
  (HSC teardown, credited to Nate Caine `natecaine@aol.com`; HSC Electronic Supply, 3500 Ryder Street, Santa Clara, CA)
- Mini-review: `https://mellottsvrpage.com/index.php/the-cybermaxx-mellott-review/` ("The CyberMaxx Review", May 2000, CyberMaxx 2.0 tested with Descent 2 vs VFX-1)

Scope of this file: everything useful about the CyberMaxx headset from those two
pages, rewritten in one place in human-readable form. No emulator code, no SDK
disassembly — just the hardware facts as published.

## 1. The 30-second version

1. The CyberMaxx is a 1990s VR helmet by **VictorMaxx Technologies** with twin
   TFT LCDs, stereo headphones, and a built-in 3DOF head tracker
   (yaw/pitch/roll).
2. It takes **either NTSC composite video or VGA RGB** in, plus stereo audio
   (dual RCA or 1x 1/8" stereo plug). Tracker data goes out over a **9-pin
   serial line at 19.2 kbaud** as ASCII `Yxxx.xPxxx.xRxxx.x` lines.
3. Stereo is **scanline-interleaved, not page-flipped**: one incoming NTSC-rate
   scanline goes to the left panel, the next to the right (15.734 kHz line
   rate, 63.5 us per line). The composite signal is still standard NTSC, so it
   can be recorded on a VCR — on a normal monitor it just looks like a blurred
   double image.
4. The VGA path is higher quality but flaky: PC software re-programs the VGA
   controller to emit the interleaved picture with NTSC timing, which breaks on
   many VGA clone chips.
5. Displays are two miniature **Matsushita MCL0712A03** TFT Active Matrix LCDs
   (camcorder-viewfinder / projection heritage) driven by one companion
   **MN83803AK** clock/driver chip plus 2 PALs that gate Hdata per panel.
   Video decode is a **Sharp IR3Y05** (NTSC-to-RGB + aux RGB input).
6. Tracker is self-contained: fluid-based gravity sensor (pitch/roll,
   +/-45 deg) + 3-axis magnetic coils (yaw 0-360 deg compass-style). One reset
   button re-zeroes to the user's rest pose.
7. Mellott's May-2000 verdict on the 2.0 unit: interesting to tinker with under
   ~$100, but bad first helmet — huge magnified optics with edge blur and
   headache after 5 minutes, front-heavy fit, almost no web docs/pinouts at the
   time. VFX-1 judged far superior (at a far higher price).

## 2. System overview

- Twin TFT Active Matrix displays + stereo headphones = live-action 3D-stereo
  video VR environment.
- 3D motion tracking unit senses head movement; host PC reads it to manipulate
  the VR view.
- Video inputs: **NTSC composite** (North American broadcast standard) **or
  VGA computer RGB**.
- Audio inputs: separate **Left + Right RCA (phono) jacks**, or one
  **1/8" stereo plug**.
- Tracker output: **serial, 9-pin connector**.
- Power: unregulated 9 VDC in (coaxial DC jack).
- All power/video/audio/tracker lines fan out through a **25-pin D breakout
  connector** (see §3), then travel over a 6-foot "umbilical" to a belt-clip
  power/audio box, then a second 4-foot cable up to the helmet.

Block diagram image on the source page: `http://mellottsvrpage.com/images/cyblk.jpg`

## 3. Breakout connector (25-pin D)

Six inputs + one output, fanned out of the single 25-pin D:

- Left Audio In (phono/RCA jack)
- Right Audio In (phono/RCA jack)
- L/R Audio In (1/8" stereo plug)
- NTSC Video In (phono/RCA jack)
- Power (coaxial DC jack, unregulated 9 VDC in)
- Tracking Comm Out (9-pin D female, serial to host)
- VGA Video In (15-pin HD male/female loop-thru)

## 4. Belt-clip power pack

- 6-foot umbilical from the breakout block routes to a belt-clip unit with
  **power supply (power switch)** + **audio amplifier (volume knob)**.
- Rails generated from the unregulated 9 VDC input:
  - **+18 VDC** — drives the TFT display panels
  - **+12 VDC** — NTSC-to-RGB decoder/driver
  - **+5 VDC** — misc logic
  - raw unregulated **9 VDC passed through** to the tracker board in the
    headset (tracker has its own regulator).
- Second **4-foot cable** from belt unit to the helmet carries everything onward.

## 5. Headset contents

Besides the stereo headphones, the helmet shell holds:

1. **Head Tracker Module** (self-contained, §6).
2. **Video Display Boards** (twin LCDs + driver/decode, §7–§9).

## 6. Head Tracker Module

### What it reports

- Continuously synthesizes helmet orientation as **roll / pitch / yaw** and
  streams it to the host PC over the comm port at **19.2 kbaud**.
- One **reset switch**: re-zeroes the unit to the wearer's rest position.
- Definitions (relative to the headset):
  - **Yaw** = turn head left/right (shaking "no").
  - **Pitch** = nod head up/down (shaking "yes").
  - **Roll** = tilt ear toward shoulder.

### Sensors

- **Gravity (pitch/roll):** vertical fluid-filled detector (page notes it was
  unclear whether capacitive or resistive). Two plate pairs vs. a fifth ground
  plate: x-axis pair (ear-to-ear) = roll, y-axis pair (front-to-back) = pitch.
  Range about **+/- 45 degrees** each.
- **Magnetic (mainly yaw):** three perpendicular coils — x-coil ear-to-ear,
  y-coil pointing out from the nose, z-coil up/down. Reads **0–360 degrees**
  like compass headings.
- Fusion rationale given on the page: on the Earth's surface the North–South
  magnetic field is crossed by the perpendicular gravity field, so 5 sensors
  (2 gravity + 3 magnetic) suffice to synthesize roll/pitch/yaw.

### Serial format

Each line looks like:

```text
Y084.7P-03.5R+04.9
```

i.e. `Y` yaw degrees, `P` pitch degrees, `R` roll degrees, one line per sample,
continuously at 19.2 kbaud. The example = facing nominally East, head slightly
down, cocked toward right shoulder (assuming zeroed level and facing North).

Emulation note: this is the single most useful tracker fact on the page —
plain-text 3-float lines over a PC serial port, no binary packet, no ISA card
needed (contrast VFX-1's ACCESS.bus-behind-VIP design).

## 7. 3D-stereo video display

### Concept

- Classic shutter-glasses stereo (SEGA / SGI + StereoGraphics style):
  left-eye and right-eye views from slightly different viewpoints, brain fuses
  to 3D. Needs fast refresh + sync so the brain sees a continuous image.
- CyberMaxx instead uses **two complete LCDs** with imaging optics aligning
  them so both images appear to come from the same place.
- A specially merged left+right video signal is sent to the display board;
  decode logic routes **alternate scanlines** to the left vs. right eye.
- A medium-resolution signal is thus decimated into two lower-resolution
  left/right images.
- Because the merged signal is still **standard NTSC composite**, it can be
  recorded/played on a normal VCR. On a regular monitor both views show at
  once as a single blurred image.

### Line rate

- Scanlines arrive at **63.5 us (15.734 kHz sweep)**: line N → left panel,
  line N+1 → right panel, alternating. The page stresses this is **not**
  NTSC interlace — it is an extra left/right interleave on top, chosen to
  minimize flicker vs. other L/R techniques.

### NTSC vs. VGA inputs

- **NTSC path:** encoded composite; compatible with anything, but quality
  suffers from NTSC encoding.
- **VGA/RGB path:** RGB direct from the host PC's VGA connector, vastly
  superior quality — but PC software must re-program the VGA controller to
  generate the interleaved picture with NTSC timing, and VGA-clone
  incompatibilities make this "questionable" per the page.

## 8. Display/driver chips

- Panels: **2x Matsushita TFT Active Matrix LCD type MCL0712A03** (originally
  for electronic viewfinders / consumer projection). Normally-white panels
  (see inversion caveat below).
- Row/column sequencing: companion **Matsushita MN83803AK** generates the
  multi-phase horizontal/vertical clocks for the on-glass shift registers.
  **One MN83803AK drives both panels** — all clock + data lines are wired in
  parallel **except one**.
- Stereo select: board logic in **two PALs** can **inhibit the Hdata signal
  to either panel** — the single per-panel address line. A panel whose Hclocks
  run but which never gets Hdata simply does not update its array. In
  Stereo3D mode each panel is inhibited on every other scanline.
- Video processor: **Sharp IR3Y05** (designed for LCD color viewfinders). Has
  NTSC-RGB decoder **plus a second aux RGB port** where the VGA signal lands.
- Adjustments actually used: **TINT** (NTSC only) and **BRIGHTNESS** (both
  NTSC and RGB). SHARPNESS, COLOR, SUB-BRIGHTNESS, GAMMA, GAIN are pinned to
  fixed values.

### Gamma

- CRTs are ~exponential (gamma ≈ 2.2) and NTSC expects that curve; LCDs have a
  more complex voltage→brightness function. The IR3Y05 has two gamma-shaping
  inputs so an LCD can mimic a CRT; without them the image washes out or
  over-contrasts depending on panel tech.
- Reference curve image: `http://mellottsvrpage.com/images/litout2.gif`

### RGB inversion quirk (flagged as a likely design bug)

- LCD pixels break down under sustained DC bias, so panels normally get AC
  drive: the IR3Y05 can invert video around Vcom toggled by the frame-rate
  pulse (FRP). Intended use: alternate whole frames invert (e.g. one pass a
  line spans 7→10 V black→white, next pass 5→2 V) so net DC ≈ 0.
- CyberMaxx instead toggles FRP **once per scanline** (presumably to cut
  flicker). Side effect: the positive-going pulse lands on one LCD and the
  negative-going pulse on the other — i.e. each panel sees a repeated DC
  polarity. The page warns anyone reusing the board with LCD pairs to bear
  this in mind.
- Diagram: `http://mellottsvrpage.com/images/videofrp.gif`

## 9. TFT panel timing (for custom drivers)

- Microscope-derived diagram on the page shows **four horizontal clock circuits
  driving three chains of R/G/B pixels in RGB-delta layout** (same-chain
  pixels offset on alternate rows).
- Custom HCP timing must delay data somewhat on alternate rows to hit the
  offset pixels correctly.
- Diagram: `http://mellottsvrpage.com/images/rgbtft.gif`

## 10. Contemporary review (May 2000, Kevin Mellott, CyberMaxx 2.0)

- Test program: **Descent 2** as the flying benchmark, compared against VFX-1.
- Price guidance at the time: worth tinkering with at **≤ $100**, more as a
  teardown curiosity than a daily driver.
- Complaints:
  1. **No docs online** — virtually nothing on pinouts/connectors; a few
     driver/game-patch pages exist, but support-wise "you are on your own".
  2. **Optics** — enormously magnified ("blocks of ice for glasses");
     headache after 5 minutes; only the center focuses, edges blur heavily
     even after adjustment.
  3. **Ergonomics** — all weight on front of head/nose; not painful but
     annoying after minutes; more time balancing head than playing.
- Bottom line quoted: don't count on long sessions "unless your blind
  already"; author considered keeping it for parts. **VFX-1 judged far
  superior — with a superior price.**
- Photo on review page: `http://mellottsvrpage.com/images/cmaxxbig.jpg`

## 11. What this means for emulation (dosbox-xr context)

- Tracker is the easy half: snoop/synthesize `Y/P/R` ASCII lines at 19200 baud
  on a virtual COM port — no ISA probing, no IRQ, no ACCESS.bus stack.
- Video is the hard half: the host must emit a scanline-interleaved NTSC-timed
  frame (odd lines = one eye, even lines = the other), either as composite or
  as a reprogrammed VGA mode. A modern XR mirror would de-interleave back into
  two eye textures rather than emulate the Matsushita/Sharp silicon.
- No I/O ports, IRQs, jumpers, or TSR drivers are documented on these two
  pages — unlike the VFX-1 VIP card. For DOS software driving CyberMaxx,
  see §12 (VR Slingshot disassembly) — vendor drivers/game patches
  mentioned but not hosted on these pages.

## 12. Consumer: VR Slingshot CyberMaxx path (moved from `VRSLING.md`)

VRS.EXE (15/10/1995, `D:\Emu\DOS\C\CY\VRSLING`, full disassembly in
`VRSLING.md`) ships a CyberMaxx tracker + video path over plain COM/VGA
ports — no ISA card, no TSR:

- Menu: `TRACKER_TYPE = CYBERMAXX` (`CB_STR_TT_CYBERMAXX`,
  `COMBO_TRACKER_TYPE`), Tracker/CyberMaxx port `COM1-4`
  (`CB_STR_MP_COM1-4`, `COMBO_CYBERMAXX_PORT`); video
  `STEREO3D_MODE = CYBERMAXX / IL320X480` (`CB_STR_S3D_CYBERMAXX`,
  `CB_STR_S3D_IL320X480`): `320x480/Interleaved` with `s` toggling 3D, or
  legacy low-res CyberMaxx mode for older displays (manual
  `README.TXT:345-376`).
- Tracker I/O: init at file `0xA9990` stores `base+0xA → [0x27450]`,
  `base+0xC → [0x27452]`, `base+0xE → [0x27454]/[0x27456]`,
  `base+0xF → [0x27458]` (same init family as modem/shutter vars);
  poll at `0xA95F0-0xA9689` is status-gated serial RX —
  `IN [0x27454], TEST 0x80` then `IN [0x27450]`, mirrored on `[0x27452]`
  with `TEST 0x80 / OUT`, plus a `CMP [0x2745A],0x400` + `0x82`
  divisor-latch-style gate before `IN [0x27458]/[0x27456]`. Packet grammar
  (expected: §6 `Y/P/R` ASCII lines at 19.2 kbaud) is parsed above port
  level and was not reversed; the port layer only proves COM RBR gated
  by LSR-style status on the Tracker COM port.
- View: F12 sets a new forward direction (game-software only, does not
  recalibrate the headtracker); keypad `*`/`-`/`+` gamma brightness;
  `320x480` interleaved video is the §7 scanline path via reprogrammed
  VGA (`0x3C4/5`, `0x3CE/F`, `0x3D4/5`, VESA `AX=4F02`), not a shutter
  port. Save writes `VRS.CFG` (`VRSC` magic).
