# CyberShades — SDK Infodump

Source tree surveyed: `D:\Emu\DOS\C\VRDRV\develop` (**every** file opened:
`CYSHSDK.TXT`, `CSHADE.C`/`CSHADE.H`, `VTIMER.C`/`VTIMER.H`, `VRI.C`/`VRI.H`,
`LCDCALL.C`/`LCDCALL.H`, `GFX.C`/`GFX.H`, `VGFX.C`/`VGFX.H`,
`DRAMGFX.C`/`DRAMGFX.H`, `GRIDEMO.C`, `TEST.C`, `OPTION.C`/`OPTION.H`,
`JOYSTICK.H`/`JOYSTICK.C`, `ISRSTUB.ASM`, `LCDBIOS.EXE` (via its API client
`LCDCALL`), `LICENSE.TXT`, plus build files `M.BAT`/`WATCOM.MAK`/`GENWAT.MAK`).
Scope of this file: **how the CyberShades glasses actually work and how
software drives them** — ports, bits, timers, video paging, and the LCDBIOS
driver API. Nothing below comes from any emulator source, only the SDK
materials. Written to match `VFX1.md` in style.

Chinon CyberShades, (c) 1995–1996 Chinon America Inc. SDK revision Beta 1
(11/8/95). Toolkit targets Watcom C/C++ 9.5b+ (386 flat model, DOS/4GW) with a
Borland 3.1 real-mode path for the LCDBIOS client.

## 1. The 30-second version

1. The CyberShades are **passive LCD shutter glasses on a plain parallel or
   serial port** — no ISA card, no VGA passthrough, no firmware handshake.
   A parallel-port Y-adapter lets a printer coexist. Image quality is
   untouched because the VGA connector is never used.
2. 3D is **frame-sequential**: left view and right view alternate on screen
   every vertical refresh (60–140 Hz), and software writes a bit pattern to
   the LPT/COM port each field to open one shutter and close the other.
3. Because most VGAs have no usable vertical-blank interrupt, the SDK
   **fakes one with the PC timer (IRQ0 / INT 8)**. It reprograms the PIT to
   an integer multiple of the measured refresh rate, fires early, busy-waits
   for vertical retrace, flips the display page, then writes the shutter
   bits. A fractional accumulator also calls the game's sound mixer at its
   own rate (typically 120 Hz) and chains the old INT 8 at 18.2 Hz so the
   DOS clock keeps working.
4. There are **two driving stacks**:
   - **Direct drive** (`VTIMER.C` + `ISRSTUB.ASM`): the game owns INT 8 and
     the shutter port. Demo: `TEST.EXE -LPT1`.
   - **LCDBIOS TSR** (`LCDBIOS.EXE` + `LCDCALL.C`): a resident driver owns
     the **RTC interrupt (IRQ8 / INT 70h, default 1024 Hz)** and the page
     flipping; the game talks to it through **`INT 10h, AX=4FD3h`**. Game
     code never touches the shutter port in this mode. `Descent` shipped
     this way so it didn't have to rewire its INT 8 sound code.
5. Video is **VGA Mode X** (mode `0x13` unchained, 320×200×256): **four 16 KB
   pages** at `A0000 + n*4000h`, used as two stereo pairs. Each timer tick
   shows `viewPage + eye` via the CRTC Start Address registers. An offscreen
   DRAM path and an LCDBIOS-buffered path render to RAM first, then copy.
6. An older, simpler API ships as `CSHADE.C`/`CS1.LIB` (devices PCP/PCS/PC3D/
   PCVR/SS1, INT 8 flip callback). The newer demo/game path is `VTIMER` +
   `VRI` + `ISRSTUB`; commercial games were told they only need those three
   modules (plus joystick).

## 2. Hardware: what the glasses are

- Shutter glasses + parallel-port driver. Software flips shutters; there is
  no readable status — it is write-only fire and forget.
- Supported ports: **LPT1–LPT4, COM1–COM4**, or none (mono preview). Chosen
  on the `TEST.EXE` command line (`-LPT1 … -COM4`, default LPT1).
- Port addresses come from the BIOS Data Area: LPT word at
  `0x408 + 2*index`, COM word at `0x400 + 2*index`. COM writes go to
  **base+4 (Modem Control Register)**, so only the DTR/RTS pins move.
- Requirements per `LCDCALL.H`: AT-class machine, 386+, register-compatible
  VGA, compatible glasses.

## 3. Shutter bit patterns

### 3a. New stack (`VTIMER.C`, what games actually use)

| Port | Left | Right | Off |
|------|------|-------|-----|
| LPT data register | `0xFF` (all bits high) | `0xFB` (`~(1<<2)`, bit D2 low) | `0x00` |
| COM MCR (base+4) | `0x01` (DTR) | `0x03` (DTR+RTS) | `0x00` |

So on LPT **only data bit D2 toggles**; on COM, RTS toggles with DTR held.
Writes are plain `outp(VSport, VSleft/VSright)` once per displayed field,
inside the timer handler *after* the retrace wait. `clearLCDs()` writes
`VSoff`. `LCDsOn()`/`LCDsOff()` just arm/disarm the per-tick write
(`flipLCDs` flag); video paging (`displayOn`) is armed separately by
`vDisplayOn()`/`vDisplayOff()`.

### 3b. Old stack (`CSHADE.H`, `CS1.LIB` device table)

| Device | Port kind | Left | Right |
|--------|-----------|------|-------|
| PCP | LPT | `0xCE` | `0xCA` |
| PCS | COM | `0x03` | `0x01` |
| PC3D | COM | `0x01` | `0x03` |
| PCVR | COM | `0x03` | `0x01` |
| PCVR | LPT | `0x04` | `0x08` |
| SS1 (Stereo Space 1) | COM | `0x03` | `0x00` |
| SS1 | LPT | `0x04` | `0x00` |

`TellGlasses()` does read-modify-write (`in` → clear mask bits → `out`) so
shutter bits share the port with other devices (docs mention PowerGlove).
Commands: `Off / Left / Right / Flip / Switch` (`Switch` inverts the
left/right assignment from then on). `StartGlasses()`/`StartMultipleGlasses()`
hook INT 8, flip all glasses, then call the game's `VGAFlipProc(State)`
callback with the new eye. Same INT 8 mechanics as below (PIT mode `0x34`,
`RESYNC_COUNT = 50` frames between re-syncs, `INTERRUPT_ADVANCE = 1000`
ticks early, DOS-tick simulation by chaining when the fractional counter
wraps, EOI `0x20 → 0x20`).

## 4. The fake vertical-retrace interrupt (direct-drive path)

`getVRR()` measures the real refresh first: latch PIT channel 0, wait two
vertical retraces (`waitVS()` polls `0x3DA` bit 3), and computes
`Hz = 1193180 / count`. The demo prints it (`VRR is %.2f Hz`).

`initLCDsys(vrr, timerAdvancePercent, vrrMultiplier, soundCallRate,
displayPageFunc, soundFunc, portInfo)` then:

1. Forces the timer multiplier up until `vrr * mult >= soundCallRate`.
   Demo defaults: `VRR_MULTIPLIER = 2`, `SOUND_DRIVER_CALL_RATE = 120`,
   `TIMER_ADVANCE = 7.16` total → `TIMER_ADVANCE_PERCENT = 7.16 / mult`
   (≈3.6 % at mult 2). The percent is empirical/user-tunable: too low =
   flicker, too high = wasted CPU.
2. Programs PIT channel 0 (`0x43 ← 0x34`, `0x40 ← timerInit`) with
   `timerInit = 1193180 / (vrr*mult*(1+advance%))` — i.e. it interrupts
   **slightly before** each field.
3. Hooks **INT 8** (`isrStub` from `ISRSTUB.ASM`: saves everything, drops to
   a 2 KB local stack, semaphore-guards reentrancy, sends EOI itself if
   re-entered). Each fire:
   - every `mult`-th tick (`displayCounter`) → `displayHandler()`: toggle
     `view` (0 = left, 1 = right), call the game's `displayPageFunc(view)`
     (`gfxPageHandler`), **busy-wait for retrace**, `resetVRI()`, reload the
     PIT, then `OUT` the shutter bits. The busy-wait time is folded back
     into the microsecond clock (`busyWaitCount`).
   - every tick → fractional sound accumulator
     (`soundCounter += 120/Hz`, call `soundFunc` on wrap — the HMI/Miles/
     Digital Expressions mixer slot).
   - `interrupted = 1` always (tells the joystick reader it was preempted),
     `intCount++` (drives `getMicroSecs()`).
4. Keeps the DOS clock alive: fractional `sim18_2 += 18.2/Hz` accumulator;
   `sim18_2wEOI()` only chains the previous INT 8 handler (or sends EOI)
   when it wraps, so ~18.2 ticks/sec still reach DOS at any PIT rate.

`deInitLCDsys()` restores the INT 8 vector, resets the PIT to 18.2 Hz
(`setTimer(0)`), restores the CRTC VRI bits, and writes `VSoff`.

Frame pacing from the game side: `waitView()` spins while `advancePage` is
set; `updateView()` advances the draw pages and sets `advancePage = 1`; the
ISR's `gfxPageHandler()` consumes it and programs the newly visible page.
Rendering never waits for the shutter — it waits for the consumed flag.

## 5. Video: Mode X pages and the eye flip

`initXmode()`: sets mode `0x13`, boosts the dot clock
(`0x3C2 ← in(0x3CC) | 4`, same trick as `IncreaseVGAFreq()`), then unchains
(`0x3C4:0x04 ← 0x0604`, `0x3D4:0x14 ← 0x0014`, `0x3D4:0x17 ← 0xE317`) and
clears all pages. Palette is gamma-corrected (`GAMMA = 1.2`, brighter helps
behind dark shutters).

Four 16 KB VRAM pages: `vmpage[n] = A0000h + n*4000h` (`n = 0..3`).
Stereo uses pairs: draw base hops `0 → 2 → 0 …` (`drawPage += 2`), and
`setDrawPage(DRAW_LEFT)` draws at `drawPage`, `DRAW_RIGHT` at `drawPage+1`.
The ISR shows `viewPage + eye` by writing the CRTC Start Address registers
(`0x3D4:0x0C/0x0D`, via `set_vpage_adr` in `VUTIL.ASM`). Mono mode walks
single pages (`+= 1`). `numPages` is always 4.

Three render back ends behind the `gfxType` vtable (`GFX.C`):
- `GFX_RENDER_VRAM` — draw straight into the Mode X pages (`VGFX.C`).
- `GFX_RENDER_DRAM` — draw into two 64 KB offscreen buffers
  (`dramPage[0/1]`, one per eye), then `d_copyPage` fans each byte out
  through the four VGA planes into the destination VRAM page
  (`DRAMGFX.C`). The `D` key toggles VRAM/DRAM live.
- `GFX_RENDER_DRAM_LCDBIOS` — same DRAM rendering, but copy + page advance
  go through the LCDBIOS calls (section 6). The `D` key cycles
  VRAM → DRAM → LCDBIOS when `-lcdbios` was given.

## 6. LCDBIOS path (the TSR `Descent` used)

`LCDBIOS.EXE` is loaded first and owns the hardware; the game links only
`LCDCALL.C`. Transport is **`INT 10h, AX = 4FD3h`**, subfunctions in `BX`:

| BX | Meaning | In/out |
|----|---------|--------|
| `3D00h` | Get version | Returns `AX=004Fh, DX=3344h, CX=4C42h` ("3D"+"LB"); `BH/BH` major/minor |
| `3D01h` | Enter 3D mode | `CX/DX` = right X/Y, `SI/DI` = left X/Y; returns `DX:DI` → flag struct |
| `3D02h` | Exit 3D mode | — |
| `3D03h` | Change start address | (alternate internal path, normally unused — the client pokes the shared buffer directly) |
| `3D04h` | Alternate→TwoBuffer setup | `CX` = 0 Full / 2 Half; returns eye column offset in `BX` |
| `3D05h` | TwoBuffer→Alternate restore | — |

Modes (`LCD3DMode`): `Off`, `TwoBuffer` (one full buffer per eye),
`AlternateFull` (one double-height line-interleaved buffer),
`AlternateHalf` (one same-height half-resolution buffer). The demo uses
`TwoBuffer` with 2 stereo pages (`lcdPages`: left `0<<14/2<<14`,
right `1<<14/3<<14` — the same 16 KB slot numbering as Mode X).

Synchronization is a shared `LCDFlags` struct in the TSR's DOS memory
(`pageAdvanced`, `interruptCount`, `delayCount`; both a real-mode far and a
flat protected-mode pointer are published):
- `pageAdvanced = 2` during the ~0.8 s calibration after entering 3D mode,
  then the RTC ISR sets it to 1 every vertical retrace once pages actually
  advance. `LCDBiosSetPage()`/`AdvancePage()` clear it to 0 and stash the
  new addresses in the shared buffer; `LCDBiosPageWait()` spins for 1.
- `interruptCount` counts RTC ticks (nominally 1024 Hz, adjustable
  512/2048/4096 via the TSR's `/FastInt:x` switch).
- `delayCount` counts frames where the TSR stalled the app (used with
  `/LockFlip`, and to invalidate joystick timing loops).

Client call order: `isLCDBiosInstalled()` → video mode set → fill
`PageAddresses` array → `LCDBiosInit3DMode()` → per frame render both DRAM
eyes → `LCDBiosPageWait()` → copy to VRAM → `LCDBiosAdvancePage()` → …
→ `LCDBiosDeinit3DMode()`. While in 3D mode the app must not touch the RTC,
the CRTC Offset / Max Scan Line / Vertical Retrace End / Start Address
registers, or flip pages itself. Joystick reads allow `INT_MASK_TIMER_0 |
INT_MASK_2ND_PIC` + `INT_MASK_RTC` in this mode (vs. timer-only otherwise).

## 7. VRI helpers (`VRI.C`)

Tiny layer over the VGA registers the timer code needs:
- `checkVRI()` — poll Input Status 0 (`0x3C2`) bit 7.
- `readVRI()/writeVRI()` — CRTC index `0x11` (Vertical Retrace End) through
  `0x3D4`.
- `initVRI()` saves the original `0x11`, then programs the
  Clear-Vertical-Interrupt bit (`1<<4`) followed by the Disable-Vertical-
  Interrupt bit (`1<<5`) sequence; `resetVRI()` re-arms each field
  (re-called if the display mode changes); `restoreVRI()` puts the saved
  bits back at shutdown. Only the status bits are ever used — the PIC is
  never unmasked for a real VGA IRQ.

## 8. Stereo math (how the two eyes differ)

Documented at length in `CYSHSDK.TXT` and implemented in `VECTOR.C` /
`GRID.C`: only the **X projection differs per eye**; Y is shared.
With eye half-separation `e/2` split into an object-space term
(`eyeObjectTrans`, applied before the divide by Z) and a screen-space term
(`eyeScreenTrans`, applied after), per eye:

`xScreen = (xObject ± eyeObjectTrans) * d / z + (xCenter ∓ eyeScreenTrans)`

The demo exposes three live knobs: `sep` (CP, screen center shift, `7/8`
keys), `msep` (VT, object-space translation, `9/0`), `zoom` (focal/scale,
`-/=` and joystick buttons). Presets 0–5 (`\` key) cover default / pop-out /
recessed × light/dark background. Sign convention: positive parallax
(left image left of right image) reads as *behind* the screen; negative as
*in front*; zero as *on* the screen. Rendering tips: never clip a
popped-out object against the screen edge, prefer low-frequency/filtered
textures (high-frequency textures shimmer differently per eye), and spend
extra pixels on **horizontal** resolution — it adds depth planes, vertical
only sharpens.

## 9. Demos, keys, and build

- `TEST.EXE [−LPTn/−COMn] [−lines #] [−size #] [−lcdbios]` (defaults:
  LPT1, 12 lines/face, size 20). `JT.EXE` is a joystick-only tester.
- In-demo keys: `S` stereo/mono, `D` render path, `B` background,
  `T` timed/polled joystick, `A` auto-rotate/joystick-rotate,
  `J` joystick readout, `7/8/9/0/−/= Kim` separation knobs, `\` presets,
  `P` pause, `Q/Esc` quit. The on-screen HUD prints `vt/cp/zm`, both
  joysticks, and fps + render-path letter (`V/D/L`).
- `M.BAT` builds `CS1.LIB` (`m clean` / `m remake`); `GENWAT.MAK` and
  `WATCOM.MAK` are the Watcom makefiles; `GO.BAT`/`D3D*.BAT` run the demo
  variants. `SIMPLE.VFT` is the vector font the HUD loads at startup.
- License: confidential Chinon devkit license — source may be built into
  3-D products in executable form only, no source redistribution, one
  backup copy, 30-day media warranty; see `LICENSE.TXT`.

## 10. Emulator fingerprints (how to recognize CyberShades software)

- **Shutter writes every field**: `OUT LPTbase, FFh/FBh` toggling bit D2,
  or `OUT COMbase+4, 01h/03h`. If only D2 of an LPT data port flips at
  refresh rate, think CyberShades (PCVR/SS1 masks `04h/08h/00h` also occur
  via the older lib).
- **PIT reprogrammed to ~2× refresh** (e.g. ~120–280 Hz) with INT 8 hooked,
  plus a fractional ~18.2 Hz chain to the old handler and a ~120 Hz
  fractional sound callback.
- **CRTC Start Address rewritten every field** (`0x3D4:0x0C/0x0D`
  alternating between 16 KB slots), with `0x3DA` bit 3 polled and CRTC
  `0x11` bits 4/5 poked — but no VGA IRQ ever unmasked.
- **INT 10h AX=4FD3h with BX=3D00h–3D05h**, success `AX=004Fh`, presence
  `DX=3344h/CX=4C42h` — the LCDBIOS API. Its shared `LCDFlags` triple
  (`pageAdvanced` 2→0→1) is the observable state machine.
- **BDA port-table reads** (`0x400/0x408` words) to resolve LPT/COM bases,
  and VGA MISC `0x3C2 |= 4` dot-clock boost at mode init.
- **Joystick** read loops that tolerate `interrupted` preemption and keep
  timer+RTC interrupts unmasked while sampling (`0x201` port reads between
  shutter writes are normal in the demo).
