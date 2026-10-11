# VR Slingshot (VRS.EXE) Hardware Addressing — Disassembly Infodump

Source tree surveyed: `D:\Emu\DOS\C\CY\VRSLING` (**every** entry opened:
`VRS.EXE` 1,043,609 bytes 15/10/1995, `VRS.CFG`/`VRSDEF.CFG` 936 bytes,
`PCWIN.DAT` Zinc 3.2 UI, `SC.DAT` sine/cosine table, `S.VFT` vector font,
`M.DAT` 65,024 bytes compressed/blob, `DMXGUS.INI` GUS patch map,
`GENMIDI.OP2` OPL patch, `320X240.256`/`320X400.256`/`320X400F.256` video blobs,
`OBJ/*.OBJ` 12 models, `TEXTURES/`, `SOUNDS/*.WAV`, `MUSIC/*.MUS`,
`MANUAL/README.TXT` + `MANUAL/VRSMAN.TXT` + `MANUAL/README.BAT`,
`VRSEXP.BAT`/`VRSME.BAT`/`VRSMEM.BAT`, `LICENSE/`).
Sibling docs: headset/tracker details live in `cybershades.md` §11
(LCD-shutter path), `VMaxxCyMaxx.md` §12 (CyberMaxx path), `IGLASSES.md`
(i-glasses! path). This file keeps the game-side index only.
Scope of this file: **how VRS.EXE addresses hardware** — ports, bits, IRQs,
video modes, and config surface. Nothing below comes from emulator sources,
only the shipped tree plus disassembly of its own binary (rizin
`rz-bin`/`rz-asm`, raw opcode scans, string census).
Toolchain: Watcom C/C++32 + Rational DOS/4GW (see §3).

## 1. The 30-second version

1. VRS is a **DOS/4GW protected-mode game** (486+, 8 MB, DOS 386-flat) with no
   ISA VR card: all VR I/O is **existing PC ports** — gameport, LPT/COM for
   shutters, COM for head trackers, COM for modem/null-modem link, SB/PAS/GUS
   + AdLib/MPU for audio. Manual (`VRSMAN.TXT:139-194`,
   `README.TXT:334-376`): 486, analog joystick (+throttle, pedals optional),
   CyberShades LCD glasses, i-glasses!/CyberMaxx tracker on a COM port,
   SB/PAS/GUS + AdLib/MPU-401/AWE32 music, 2400 bps–28.8k modem or 2-3-7
   null-modem.
2. **Stereo is a menu choice, not auto-detect**: `STEREO3D_PORT = NONE /
   LPT1-4 / COM1-4`, `STEREO3D_MODE = NONE / LCD / CYBERMAXX / IL320X480 /
   SIDEBYSIDE / TOPBOTTOM / NTSC_FS`, plus `Mode 1 / Mode 2` retrace-sync,
   `VideoSync`, `StereoAdvance`, `ReverseStereo`, `INT_VSYNC`. Shutter and
   interleaved/CyberMaxx semantics live in `cybershades.md` §11 and
   `VMaxxCyMaxx.md` §12; game-side keys/vars only here.
3. **Tracker is a menu choice**: `TRACKER_TYPE = NONE / CYBERMAXX /
   I_GLASSES`, `CYBERMAXX_PORT = COM1-4`. Tracker semantics live in
   `VMaxxCyMaxx.md` §12 (CyberMaxx) and `IGLASSES.md` (i-glasses!);
   game-side keys/vars only here. F12 re-zeroes view in software only.
4. **Joystick is direct gameport**: `mov edx,0x201; out dx,al; in al,dx`
   trigger + `rcr` nibble loop at file `0x7D16E` (second path `0xAC1DE`
   adds PIC `0xA1` mask read). Cal values live in `VRS.CFG` (`INT_JX_*`,
   `INT_RUDDER_*`, `INT_THROTTLE_*`, null zone, sensitivity).
5. **Sound is direct + middleware**: AdLib status `in al,0x388` at `0xAC635`;
   SB base validated `0x210-0x280, & 0x0F` at `0xA9A11`, derived
   `0x220/226/228/229/22B/22F` family stored to `0x2728x-0x2729x` at
   `0xA5657`, DSP/mixer traffic at `0xA5480/0xA5860`; GUS/PAS/AWE strings
   (`Ultrasound @ Port:%03Xh, IRQ:%d, DMA:%d`, `PAS IRQ: %d DMA:%d`,
   `GENMIDI.OP2`, `NO MVSOUND.SYS`) + README note (newer Digital Expressions
   driver) mean GUS/PAS go through that driver, not fully reversed here.
6. **Serial/modem is direct 16550**: COM base table at linear `0x27424`
   (`mov dx,[ebx*2+0x27424]`), LSR/MSR/MCR-style offsets `+4/+5/+6`, divisor
  /`IER`/`IIR` patterns (`0x82`, LCR `0x80`, `IER 0x01`, `IIR+0x0A`,
   `LSR&0x40`), UART-type strings (`8250/16450`, `16550`, `None`,
   `PROMPT_UART`), full AT dial/answer/COM-rate/prefetch UI with CRC/timeout/
   carrier errors. One COM variable (`0x27436`) is stored XOR `0xB8B`
   (`mov dx,[0x27436]; xor edx,0xB8B; and edx,0xFFFF; in al,dx`).

## 2. Docs: what the manuals say the hardware is

- Requirements (`VRSMAN.TXT:139-194`): 486 (66 MHz if stereo/HMD), analog
  joystick (CH FlightStick w/ throttle used for testing; clean pots
  required), rudder pedals optional, 2400+ bps modem (14.4k recommended),
  null-modem = wires 2-3-7 crossed, modem cable = straight 2-8+20 with
  TXD/RXD/GND/DCD/RTS/CTS/DTR functional, CyberShades glasses for 3D,
  SB/PAS/GUS + stereo amp/headphones for 3D sound.
- Shutter setup (`README.TXT:274-343`): Hardware Setup names the serial or
  parallel port the glasses use; stereo modes + frame selection tried live;
  `Mode 1` (sync on retrace before field advance, most common) vs `Mode 2`;
  `s` toggles field-sequential; `7/8` separation, `9/0` inter-ocular angle,
  `-`/`=` zoom, `\` reset. `README.BAT`/README top: `Video Sync Advance` —
  raise when stereo unstable, lowest stable value = fastest.
- Tracker/HMD (`README.TXT:345-376`): File/Hardware Setup → Tracker Port
  (COM) + Tracker Type (i-glasses! or CyberMaxx); `320x480/Interleaved`
  (`s` toggles 3D) or legacy low-res CyberMaxx mode; F12 software-only
  forward view; keypad `*`/`-`/`+` gamma. Details → `VMaxxCyMaxx.md` §12,
  `IGLASSES.md`; shutter details → `cybershades.md` §11.
- Sound/music (`VRSMAN.TXT:411-455`, `README.TXT:60-69`): WAV list
  (`BOUNCE GAME_STA AIRBALL ... SHIPBUMP GAME_WAI ENERGY_U`) replaceable;
  music = `MAIN.MUS/GAME1.MUS/GAME2.MUS`, MID→MUS via `midi2mus.exe`;
  textures = 128×128 256-color PCX (`BACK1/2 SHIP1T/1B SHIP2T/2B`) with VRS
  palette preserved. Music cards: AdLib, MPU-401, Ultrasound, AWE32.
- Modem init (`VRSMAN.TXT:254-410`): direct async, no buffering, RTS/CTS on,
  CD tracks carrier, DTR hangup, word responses; examples
  Practical `AT&F1&Q0` / `AT&C1&D2&K3&Q0`, Supra `AT&F2&Q0` / same,
  ZyXEL `AT&F&M2` / `AT&C1&D2&H3&M2`; shipped default in `VRS.CFG` =
  `AT&Q0&C1&D2&K3` (Supra-family subset). S10=255 anti-hangup, Xn blind
  dial, prefetch 1 (fast/direct) to 2-4 (slow/noisy), sync-delay,
  command/connect timeouts.

## 3. Binary identity

- `rz-bin -I`: MZ 16-bit, i386, DOS; sections `seg_000-003` + huge
  `overlay @0xF474 size 0xEF825`. Overlay magic `42 57 E0 01`, no `LE`
  header at stub end — Rational DOS/4GW image, not raw-LE.
- Strings: `DOS4GW.EXE`, `DOS/4GW Professional Protected Mode Run-time`,
  `Rational Systems 1987-1993`, `WATCOM C/C++32 1988-1993`,
  `DOS16M.386` stub texts, `SWMODE=`/`INBOARD`/`DOS16M=` env. So: Watcom
  32-bit game + DOS/4GW loader; linear vars cited below (`0x272xx`,
  `0x274xx`) are DOS/4GW linear addresses, resolved with
  `rz-asm -a x86 -b 32`.
- CLIs: `VRSEXP.BAT` = `vrs`; `VRSME.BAT` = `vrs d`; `VRSMEM.BAT` =
  `vrs d c` (extra DOS-memory flags for low-memory machines).

## 4. Config surface (what the UI stores)

String census (all `CB_*` are Zinc `PCWIN.DAT` widgets):

| Group | Values | Source strings |
|---|---|---|
| Serial (link) port | NONE, COM1-4 | `CB_STR_SP_COM1-4`, `COMBO_SERIAL_PORT` |
| Stereo port | NONE, LPT1-4, COM1-4 (semantics → `cybershades.md` §11) | `CB_STR_S3D_LPT1-4`, `CB_STR_S3D_COM1-4`, `COMBO_STEREO3D_PORT` |
| Stereo mode | NONE, LCD, CYBERMAXX, IL320X480, SIDEBYSIDE, TOPBOTTOM, NTSC_FS (semantics → `cybershades.md` §11, `VMaxxCyMaxx.md` §12) | `CB_STR_S3D_*`, `COMBO_STEREO3D_MODE` |
| Tracker type | NONE, CYBERMAXX, I_GLASSES (semantics → `VMaxxCyMaxx.md` §12, `IGLASSES.md`) | `CB_STR_TT_*`, `COMBO_TRACKER_TYPE` |
| Tracker/CyberMaxx port | NONE/COM1-4 (semantics → `VMaxxCyMaxx.md` §12, `IGLASSES.md`) | `CB_STR_MP_COM1-4`, `COMBO_CYBERMAXX_PORT` |
| Sound card | NONE, SOUND_BLASTER, PAS, GUS | `CB_STR_SC_*`, `COMBO_SOUND_CARD` |
| Music card | NONE, ADLIB, AWE32, GUS, MPU401 | `CB_STR_MC_*`, `COMBO_MUSIC_CARD` |
| UART | 8250/16450, 16550, None | `UART:*`, `PROMPT_UART` |
| Knobs | VideoSync, StereoAdvance, ReverseStereo, VSYNC, ComRate, ComSetup, JoystickCal, prefs/prefetch/timeouts/volumes | `CB_BTN_*`, `INT_*`, `GROUP_*` |

`VRS.CFG`/`VRSDEF.CFG`: magic `VRSC\0\0`, 936 bytes, identical except
played-state; embeds `New Player` name + default modem init
`AT&Q0&C1&D2&K3` in cleartext at tail. Joystick/rudder/throttle
min/max/in/out, null zone, sensitivity, stereo sep/SIA/zoom are binary
ints ahead of it (widget keys `INT_JX_MIN/MAX/IN/OUT` etc. name them).

## 5. Port census (BA-proximate IN/OUT DX, whole image)

`mov edx,imm` → IN/OUT within 64 B histogram (779 total DX-IOs; top):
`0x3CE×98 0x3C4×75 0x3C5×62 0x021×39 0x3D4×31 0x020×21 0x002×20 0x3DA×16
0x3CF×16 0x040×15 0x061×13 0x3D5×12 0x3C0×11 0x3C2×9 0x0A1×8 0x043×7
0x201×3 0x388×1` (+ `0x210×2 0x300×1 0x030/034/00A/00C` DMA/PIC-adjacent,
`0x060/064` keyboard, `0x0D4/D8/0A/0C` DMA2). COM/LPT/SB-data paths are
memory-indirect (`mov dx,[0x27xxx]`) so they do not appear here — see §6-§8.

## 6. Input: joystick / rudder / throttle / keyboard / mouse

- Gameport trigger+read, file `0x7D16E` (bytes
  `BA 01 02 00 00 EE EC F6 D0 C1 E8 04 …`):
  `mov edx,0x201; out dx,al; in al,dx; not al; shr eax,4;
  mov [0x5674],eax; in al,dx; …; rcr al,1; adc [0x5664/68/6C/70],ebx;
  loop` — classic 4-axis + 2-button bit-bang; zeros `0x5664-70` first.
- Second path `0xAC1DE`: same `mov edx,0x201` masked `&0xFFFF/&0x0F`,
  then `mov edx,0xA1; in al,dx` (PIC2 mask snapshot) and delay loop —
  joystick read that snapshots IRQ masking (shared with sound-link code).
- Keyboard: `0x60` (data, `0x71716`), `0x64` (status, `0x8070E` etc.),
  `0x20/0x21/0xA0/0xA1` PIC mask/EOI around handlers; `INT 09h×6`,
  `INT 08h×6` (timer) in census. Keys per manual: `s` stereo, `7/8/9/0/-/=`
  + `Y/U/I/O/P/[/]` views, `F12` tracker-forward, keypad gamma, `a`
  autopilot, `q` quit, arrows/`z/x/c/v/b`, `Ctrl/Alt`, `m , . /`, Home/Ins.
- Mouse: `Error initializing mouse: %d`, `MOUSE`, `INT 33h×7`; right-button
  menu (`Config/ComRate/Practice`) is Zinc, not hardware.

## 7. Stereo/trackers: game-side index (details moved out)

Headset/tracker semantics now live in `cybershades.md` §11 (LCD shutter),
`VMaxxCyMaxx.md` §12 (CyberMaxx), `IGLASSES.md` (i-glasses!). Game-side
facts kept here:

- Shared port-var init at file `0xA9990`: input `EAX` = selected base;
  stores `[0x27448]=base+0, [0x2744A]=base+4, [0x2744C]=base+5,
  [0x2744E]=base+6, [0x27450]=base+0xA, [0x27452]=base+0xC,
  [0x27454]=base+0xE, [0x27456]=base+0xE(dup), [0x27458]=base+0xF`.
  Shutter routine `0xA983E`, tracker poll `0xA95F0-0xA9689`, COM table
  `0x27424` (`[ebx*2+0x27424]`, EBX = COM1-4) at `0xA8B03/0xA8BE9`,
  obscured COM var `0x27436` (`^0xB8B`, `0xA889A`) — see §8 for the modem
  half of the same machinery.
- User keys: `s` 3D toggle, `7/8` separation, `9/0` inter-ocular,
  `-`/`=` zoom, `\` reset, F12 software forward view, keypad gamma.

## 8. Serial link / modem (head-to-head play)

- Same COM-table core as §7 plus modem base `0x27448` word paths at
  `0xA9718-0xA97A6`:
  `mov dx,[0x27448]; add edx,4; in ax,dx; and eax,0xFF00; cmp 0x4500`
  (MSR-style +4 check), `mov dx,[0x27448]; out dx,ax` (THR),
  `add edx,2; in ax,dx; and dh,0x40` (LSR-style +5/+2 with `0x40` TEMT
  mask), `cmp 0x132` baud-gate, `out dx,ax` divisor `0x01/0x15`.
  MCR/LSR OUT/IN bytes at `0xA9845/0xA9863` (`[0x2744A]/[0x2744C]`).
- UI/errors: `A serial port CRC error / send|read timed out`,
  `The modem link has been aborted / did not respond / didn't connect /
  response too long|invalid`, `The Modem Carrier is not present`,
  `The serial port hasn't been selected`, `CB_STR_ModemInitString`,
  `INT_COMMAND/CONNECT_TIMEOUT`, `INT_SYNC_DELAY`, `Syncing/Retry/Start
  Sync`, UART `8250/16450 / 16550 / None` probe result.
- Line discipline per manual: DTE=connect rate, buffering off, RTS/CTS +
  CD + DTR必需; prefetch trades lag vs frame rate (30 fps capable).

## 9. Video (VGA reprogramming, no HMD video card)

- Misc/dot-clock `0x73D16`: `in al,0x3CC; …; out 0x3C2,al|0x04`
  (same `MISC|=4` boost as CyberShades `initXmode`).
- Retrace wait `0x74EEC`: three `in al,0x3DA; test al,0x08` loops
  (`jz/jnz` pairs) + `0x74F61/0x75028/0x75066` `OUT 0x3DA` shims —
  polled status bit 3, never a VGA IRQ (matches `cybershades.md` §7: PIC
  never unmasked for VGA).
- Sequencer/GFX/Attr/Palette storm `0x75000-0x79FFF`:
  `0x3C4/3C5`, `0x3CE/3CF`, `0x3C0`, `0x3C8/3C9`, `0x3C7`, `0x3CD`,
  CRTC `0x3D4/3D5`, status `0x3DA/0x3BA`, mono `0x3B4/3B8/3BF` — Mode-X-ish
  unchain + start-address paging for page-flip/interleave; palette via
  `INT 10h AX=0x1000-0x1010` family (81 INT-10h sites).
- VESA: `mov ax,0x4F02` at `0xE56C8`/`0xE585E` (`66 B8 02 4F`) with
  `BX` mode select — SVGA banked path for `IL320X480`/`SIDEBYSIDE`/
  `TOPBOTTOM`/`NTSC_FS`; local blobs `320X240.256`/`320X400.256`/
  `320X400F.256` + string refs `320x240.256` etc. at `0xEBDDC`.
- `INT_VSYNC` + `VideoSync`/`StereoAdvance` = software-phase trim around
  the `0x3DA`-poll, same role as CyberShades `TIMER_ADVANCE`.

## 10. Audio (SB/AdLib direct, GUS/PAS via driver)

- AdLib `0xAC635`: `mov edx,0x388; sub eax,eax; in al,dx`
  (status read; write paths adjacent use `0x388/0x389`-family stores).
- SB validate `0xA9A11`: `mov eax,0x41; call; cmp ax,0x210; jb; cmp
  0x280; ja; test al,0x0F; jz → mov edx,0x220` — base must be
  `0x210-0x280` on a 16-boundary (covers `0x220` default + `0x240` etc.).
- SB derive `0xA5657`: `mov edx,0x220; sub eax,0x220; add edx,eax` then
  `0x226/228/229/22B/22F` each `add edx,eax; mov [0x27288/98/9E/96/…],dx`
  — classic DSP `2x6/2x8/2x9/2xB/2xF` family from jumpered base.
- SB DSP/mixer `0xA5480/0xA5860`: `mov dx,[0x27298]; out dx,al(0x04)` +
  `mov dx,[0x2729E]; out dx,al` (mixer index/data), `0x05 →
  [0x27294]` + data `[0x2729A]/[0x27296]` sequences with `|0x80/|0x40`
  masks, `cli` guards, `0x2723C` patch byte — mixer-volume path behind
  `INT_SOUND_VOL`/`INT_MUSIC_VOL`.
- GUS/PAS/MIDI (driver-level, not port-reversed): `Ultrasound @
  Port:%03Xh, IRQ:%d, DMA:%d`, `PAS IRQ: %d DMA:%d`,
  `Pro Audio Spectrum is NOT version 1.01`, `Pro Audio Spectrum 3D JAZZ`,
  `GENMIDI.OP2`, `NO MVSOUND.SYS`, `midi\`, `CB_STR_MC_AWE32/GUS/MPU401`,
  `CB_STR_SC_GUS/PAS/SOUND_BLASTER`. README: Digital Expressions driver —
  treat as middleware config (port/IRQ/DMA prompt), not direct DSP code.
- PIT/speaker `0x75446/0xA3855`: `out 0x43,al; in al,0x40` pairs (timer
  read), `in/out 0x61` + `out 0x43/0x42` (speaker gate + counter 2) —
  sound timing + PC-speaker path.

## 11. System glue (PIC/DMA/timers/interrupts)

- PIC `0x728B8/0x710EA/0x72AA3/0xAA674`: `mov edx,0x20; mov al,0x20;
  out dx,al` (master EOI), `0x21/0xA1` mask RMW loops, `0xA0` cascade —
  joystick/sound/serial handlers EOI, never VGA.
- DMA `0xAC797`: `mov edx,0xD4; out dx,al(0x04|idx)` mask,
  `mov edx,0xD8; xor al,al; out` clear, `0x0A/0x0C` single-mask/clear —
  16-bit DMA2 (GUS/SB-16-style) + low-DMA mirrors; `0x00A/00C/00B`
  (`0xAC7AF-0xACA0A`) = DMA1 status/mask cluster.
- PIT `0x75446/0x885D8`: `out 0x43; in 0x40` ×2 + `shl/or` 16-bit tick —
  frame/sound pacing; `Error installing timer service` string.
- INT census: `21h×266` DOS, `31h×142` DPMI, `10h×81` video,
  `33h×7` mouse, `2Fh×19` mux, `15h×17`, `67h×12` EMS, `08h/09h` timer/kbd.
- Zinc UI owns menus/dialogs (`PCWIN.DAT`); game loop never touches disk
  mid-frame except demand-load (`DEMAND LOAD: %lX => %s Obj %d Page %d`).

## 12. Emulator fingerprints (how to recognize VRS)

- `OUT 0x201` then 1-2 `IN 0x201` + `RCR/ADC` burst storing to 4 adjacent
  dwords (axes) — joystick, not PGC/PowerGlove (no `0x378/0x379`
  bit-bang, no `0x0A/0x05` echo).
- `IN 0x388` + SB `0x22x` family derived from a validated base with
  `&0x0F` alignment check; `GENMIDI.OP2` + `Ultrasound @ Port` prompts.
- COM `RBR/IER/IIR/LCR/MCR/LSR/MSR` (`+0..+6`) traffic from the
  `[ebx*2+0x27424]` table + `^0xB8B`-masked `[0x27436]` read; shutter OUTs
  and tracker `test 0x80` gated INs share the `0xA9990` derived-var
  family — semantics per `cybershades.md` §11, `VMaxxCyMaxx.md` §12,
  `IGLASSES.md`; modem AT/`CONNECT`/CRC/carrier strings nearby.
- VGA `0x3CC→0x3C2|0x04`, `0x3DA` bit-3 spin ×3, `0x3C4/3C5/3CE/3CF/3C0`
  storm, `INT 10h AX=0x4F02` VESA set + `AX=0x1000`-family DAC —
  interleaved/page-flip stereo, **not** CyberShades per-field LPT `FF/FB`
  (that pattern does not appear here).
- Config always nearby: `VRSC` magic, `New Player`, `AT&Q0&C1&D2&K3`,
  `CB_STR_S3D_* / CB_STR_TT_* / CB_STR_SC_* / CB_STR_MC_*`.

## 13. File-by-file inventory (hardware relevance)

- `VRS.EXE`: all §5-§12. Watcom32 + DOS/4GW; no private ISA VR card code.
- `VRS.CFG`/`VRSDEF.CFG` (936 B, `VRSC`): port/mode/type selections,
  joystick cal, volumes, timeouts, name, modem-init. Defaults = VRSDEF.
- `PCWIN.DAT` (70,144 B, `Zinc Data File Version 3.2`): menu/dialog
  geometry incl. `WINDOW_JOYSTICK_CAL`, `WINDOW_COM_SETUP` — UI only.
- `DMXGUS.INI`: GUS patch-size → patch-name map (256K-1024K columns) for
  the middleware patch loader.
- `GENMIDI.OP2`: OPL3 instrument patch (AdLib music voice data).
- `320X240.256`/`320X400.256`/`320X400F.256`: video-mode blobs
  (resolution-named; exact struct not decoded — mode tables/palettes).
- `OBJ/*.OBJ` (12: SHIP/CYBALL/GOAL/GRAVITRO/HANGER/MAINPAD/LIGHTS/
  SURFACE1-4/TRACKER): 3D meshes, no I/O.
- `TEXTURES/*.PCX`, `SOUNDS/*.WAV`, `MUSIC/*.MUS`: replaceable assets
  (128×128 PCX w/ VRS palette; WAV SFX; MUS songs via `midi2mus`).
- `SC.DAT` (57,600 B): int16 sine/cosine ramp table (physics LUT).
- `S.VFT` (10,696 B): vector-font glyph deltas (HUD text).
- `M.DAT` (65,024 B): opaque compressed/blob (high-entropy, no strings
  of hardware interest; likely missions/packed assets).
- `MANUAL/README.TXT`: install/memory/boot-disk (§1-115), stereo/tracker
  setup (§274-371; semantics moved to `cybershades.md` §11,
  `VMaxxCyMaxx.md` §12, `IGLASSES.md`), order form. `MANUAL/VRSMAN.TXT`: full 48-page manual
  (hardware `§139-210`, modem `§254-410`, sound/texture swap `§411-461`,
  joystick cal `§490-530`, stereo keys `§531-636`, head-to-head `§1209+`).
  `MANUAL/README.BAT`: print helper. BATs: `vrs` / `vrs d` / `vrs d c`.

## 14. Claim → source map

- Manual hw/modem/sound/tracker/shutter/keys → `MANUAL/VRSMAN.TXT`
  `§139-210/254-461/490-636/1209+`, `MANUAL/README.TXT` `§274-376`.
- EXE size/date/stub/extender → `VRS.EXE` stat + `rz-bin -I`,
  strings `DOS4GW.EXE/Rational/WATCOM/DOS16M.386/SWMODE=`.
- Menu enums → `VRS.EXE` strings `CB_STR_SP_/S3D_/MP_/TT_/SC_/MC_*`,
  `COMBO_*`, `Mode 1/2`, `UART:*`, `PROMPT_UART`, `INT_VSYNC`,
  `CB_BTN_VideoSync/StereoAdvance/ReverseStereo`.
- CFG magic/content → `VRS.CFG` hex `56 52 53 43…New Player…AT&Q0&C1&D2&K3`.
- Joystick `0x201` → file `0x7D16E` (`BA 01 02 EE EC … RCR/ADC×4`),
  `0xAC1DE` (`BA 01 02 … BA A1 EC`).
- AdLib/SB → `0xAC635` (`BA 88 03 EC`), `0xA9A11`
  (`CMP 210/280, TEST 0F → 220`), `0xA5657`
  (`220/226/228/229/22B→[2728x-9x]`), `0xA5480/0xA5860` (mixer).
- GUS/PAS/AWE/MPU → strings `Ultrasound @ Port…/PAS IRQ…/GENMIDI.OP2/
  NO MVSOUND.SYS/CB_STR_MC_*` + README Digital-Expressions note.
- COM/table/XOR → `0x27424` (`8B 14 5D 24 74 02 00`), `0x27436`
  (`81 F2 8B 0B 00 00`), modem `0x27448` block `0xA9718-0xA97A6`,
  shared init `0xA9990`, shutter `0xA983E`, tracker `0xA95F0-0xA9689`
  (semantics → `cybershades.md` §11, `VMaxxCyMaxx.md` §12, `IGLASSES.md`).
- VGA/PIT/PIC/speaker/DMA/INT → §5 census + `0x73D16/0x74EEC/0x75446/
  0x728B8/0xAC797/0xA3855`, INT histogram (`CD 21/31/10/33`),
  VESA `66 B8 02 4F` ×2, mode blobs `320X*` + `0xEBDDC` refs.
