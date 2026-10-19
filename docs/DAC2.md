# Dactyl Nightmare 2 — Technical Infodump

Source trees surveyed (both dumps carry a `DN2` install; they are DIFFERENT
builds, see §3):
- `D:\Emu\DOS\C\V2k\VR 2000 Series Disc Image (Simon's)\DN2\`
  (`DN2.EXE` 947040 B, `DN2C.EXE` 760096 B, `DN2.INF`, `DN2RUN.BAT`,
  `MAINA.OUT`, `MAINB.OUT`, `SNDSCAPE.COD`, `BILL1-4.TEX`)
- `D:\Emu\DOS\C\V2k\VR2000 HDD Dump (Faulty's)\DN2\`
  (`DN2.EXE` 851144 B, `DN2C.EXE` 674212 B, `DN2RUN.BAT`, `EXPEXIT.BAT`,
  `RNT`, `MAINA.OUT`, `MAINB.OUT`, `SNDSCAPE.COD`, `BILL1-4.TEX`,
  `RUNFILES\` with `DACFLIC.FLC`, `DATABASE.DN2`, `DEFAULTS.DN2`,
  `HISCORES.DN2`, `LOOK.DAT`, `LOOKCAM.DAT`, `SOUND.DN2`, `TEXTURES.DN2`,
  `VOLUMES`, `VOLUMES.DN2`)

Method: `rz-bin -I/-S/-e/-zq` (Rizin) for structure, raw-ASCII scan
(2459 printable runs ≥6 in Simon's `DN2.EXE`) for behavior, `rizin -q`
bounded entry disassembly, opcode/INT byte census (heuristic — opcodes
also occur as data, same caveat as `SU2000.md` §11). Full linear
disassembly infeasible at this size (same wall ETS hit). Nothing below
comes from any emulator source.

## 1. The 30-second version

1. DN2 ("Dactyl Nightmare 2 - The Race For The Eggs") is a 2-player
   flying-shooter VR experience: race for eggs, dactyls (HUNGRY/COME/HYPER/
   PHAZE/GURU/DAC/BULL volumes), 2-player + 4-player lock, difficulty
   levels, hiscores with 3-letter initials.
2. Both dumps ship it but the builds differ: Simon's `DN2.EXE` is
   experience **v02.05.00** (TRK 01.04.42, CTRL 01.03.51, late-1995 libs);
   Faulty's is **v02.04.00** (TRK 01.04.16, CTRL 01.03.40, early-1995 libs).
   `DN2C.EXE` is the camera variant (same game version, same libs
  except STAT — has VEL/NET-lib/PIX-lib/TRK/SND/CTRL/CD/MISC/EXP).
3. Tracker lib is NEWER than ETS's (01.04.42 vs 01.04.35) and stricter:
   `TRK BAD VERSION` rejection, `trkdelay`/`trkreset` env vars, 2000 ms
   default startup delay, metal compensation per sensor.
4. Network magic is `DAC2` (3× adjacent slots + `Initialising 2-Player
   game`), matching the `01:44:41:43:32:00` 131 B frames captured live in
   `dumps_network.html` — DN2 never says `ZONEH`.
5. PIX side is dual-MC88110 with a `PixMon>` monitor (`Processor A/B`,
   `SB addr` select, `Could not find processor card at 0x%lX`); Faulty's
   `RNT` is a real PixMon session transcript (failed `file`/`list`/`dir`
   probes included).
6. Sound is dual-Soundscape (`sound1`/`sound2`, DMA, `SNDSCAPE.COD`
   firmware download with per-sample logging) with a dactyl-specific
   volume table (`DACVOL/BULLVOL/HUNGRYVOL/...`).

## 2. Install layout + launch chain

- `DN2.INF`: `NAME Dactyl Nightmare 2` (`CODE` default) + `DN2C`
  `Dactyl Nightmare 2 Camera`. `DN2RUN.BAT` = `dn2` (Simon's, 12 B);
  Faulty's variant is 8 B + ships `EXPEXIT.BAT`.
- `SNDSCAPE.COD` 61440 B in both (same size as ETS/ZONE copies — the
  shared Soundscape firmware blob).
- `BILL1-4.TEX` 32816 B each at DN2 root (billboard textures; `.TEX`
  matches the `files/dacwall.tex` format noted in `SU2000.md` §9).
- `MAINA.OUT`/`MAINB.OUT` (63–70 KB) are Watcom linker map/response
  files, not game logs: custom segments `.vecs/.text/.data/.bss/.model/`
  `.texture/.bitmap/.font/.global/.fmvmem/.pmem` — the PIX content
  pipeline (textures/bitmaps/fonts/FMV memory) is linker-resident.
- Faulty's `RNT` (186 B) is a PixMon transcript, not a config:
  `PixMon> file dnt` → `ERROR : File currently open.` etc.
- Faulty's `RUNFILES\`: `DACFLIC.FLC` 4759750 B (FMV clip),
  `DATABASE.DN2` 18194 B, `DEFAULTS.DN2` 33 B (difficulty/config numbers:
  `3 10 12 15 120` + four `1/0` toggles), `HISCORES.DN2` 270 B (20 rows
  of `initial-initial-initial score`, top `3 13 15 10000`),
  `LOOK.DAT`/`LOOKCAM.DAT` (look-at waypoint rows: pos + target +
  ranges, e.g. `35000.0/7000.0` arena scale), `SOUND.DN2` 3220 B,
  `TEXTURES.DN2` 10198 B, `VOLUMES` 5921 B (same shape as ETS `TESTVOLS`),
  `VOLUMES.DN2` (per-sound volumes, §7).

## 3. Binary identity + build drift

- MZ + `LE` (e_lfanew → `LE`), Watcom 10.0a + Rational DOS/4GW, 80386,
  static, VA. Code object 1: 0x96000 B. Entry `0x68420` = `jmp 0x68498`
  stub (data skipped over, same shape as the MUTE entry sample in
  `SU2000.md` §11).
- Raw opcode census (heuristic): `EC` 4282 / `E4` 1715 / `EE` 961 /
  `ED` 722 / `EF` 565 / `E6` 616 — heavy port IO for a game (tracker +
  PIX + SND + NET all port-driven). INT census: `0x21` 116 (DOS),
  `0x10` 76 (video), rest data noise.
- Version matrix (Simon's `DN2.EXE` v02.05.00 vs Faulty's v02.04.00):

  | Lib | Simon's (late 95) | Faulty's (early 95) |
  |---|---|---|
  | DN2 experience | v02.05.00 | v02.04.00 |
  | EXP | 01.02.26 (Oct 28 95) | 01.02.23 (Jan 04 95) |
  | PIX (monitor) | Jun 08 95 | Feb 21 95 |
  | TRK | 01.04.42 (Oct 20 95) | 01.04.16 (Jan 05 95) |
  | CTRL | 01.03.51 (Nov 17 95) | 01.03.40 (Dec 02 94) |
  | CD | 00.01.34 | 01.27.00 (!) |
  | MISC | 00.01.07 | 00.01.05 |
  | SND | 00.01.56 | 00.01.49 |
  | NET | (Nov 14 95) / lib v00.02.31 | — |
  | STAT | 00.01.13 | 00.01.13 |
  | VEL | 01.00.00 (Jun 30 95) | 01.00.00 (Feb 13 95) |
  | PIX lib | v01.05.65 | — |

  `DN2C.EXE` (camera) reports the same game version with the same libs
  except STAT (has VEL/NET-lib/PIX-lib/TRK/SND/CTRL/CD/MISC/EXP).

## 4. Tracker usage (TRK 01.04.42 — strictest lib seen)

- Open flow strings (same `W`/`S` protocol ETS uses, `SU2000.md` §8):
  `Trying to open card %ld as an InsideTRAK at address 0x%lx`,
  `InsideTRAK string not found in correct place` (×2 sites),
  `Opening tracker library v%s`, `Tracker Library Open complete`,
  `Closing tracker library` / `Closing TRK library`, `Cannot open
  tracker`, `Tracker not open/responding`, `Sensor not active`,
  `Illegal tracker type/sensor value`, `TRK ERROR (%ld)`.
- **New vs ETS's 01.04.35**: `TRK BAD VERSION` (firmware-version gate —
  the sign-on must satisfy it, not just contain the substring),
  `trkdelay` env (`Tracker delay specified and set to %d`,
  `... NOT specified using the default: (2000)` — 2000 ms startup
  delay), `trkreset` env (`Tracker reset environment variable`),
  `About to set sensor offset/world offset/frequency/metal
  compentsation [sic]`, `Set metal compensation for sensor %ld.
  Parallel %7.3f, perpendicular %6.3f`, `Illegal sensor number` (×2),
  `Tracker Reference Frame required`, `Getting raw tracker data for
  card %d, max size of %d`, `All done getting data`,
  `Set sensor use to %x`.
- Config grammar (same `KEYWORD.VPC` set): `TRACKER`, `TRACKER%ld`,
  `SENSOR%ld`, `FASTRAK`, `INSIDETRAK`, `NO_TRACKER`,
  `RANDOM_TRACKER1`, `FIXED_TRACKER`, `SENSOR_OFFSET`, `HEMISPHERE`,
  `SENSOR_MAP`, `SENSOR_DATA`, `CONSOLE_TYPE`.
- Logging: `trk.log` + `Serial track not found` (serial fallback probe).
  (CD layer reuses `Track %02d : Start/End` for *disc* tracks — same
  word, different device; `Attempt to access an invalid track number`,
  `Track cache failed... Resetting drive (NOT)`.)

## 5. Control / FORMAT usage

- `Cannot open CTRL library/format card`, `CTRL library not open`,
  `CTRL hardware not responding`, `Illegal joystick/format`,
  `Format C/D not found`, `Backlight disabled`, `CTRL ERROR (%ld)`.
- Credit system: `Credits`, `continual credit`, `%ld keyboard
  credit%s`, `Credit on credit device`, `No credit on credit device`,
  `Not checking credit device`.
- Backlights: `Backlight 0/1 = %d`, six `BackLight %d = %-3d` slots,
  `[BACKLIGHTS]` config key.
- 2-player link modes: `Setting SINGLE_PLAY/LINK_UP for
  CTRL_PLAYER1/2 to %s`.
- Config keys: `[CTRL]`, `FORMAT_CARD`, `NO_FORMAT`, `FORMAT`.
- Sound-name leakage in CTRL: `MICNET`, `VISETTE`, `VISETTE_MASTER`
  (shared volume namespace, §7).

## 6. PIX / video usage (dual-88110 + PixMon)

- Mode enum (all 32): `PIX_PAL/WIDEPAL/NONPAL/WIDENONPAL/NTSC/WIDENTSC/
  NONNTSC/WIDENONTSC/LCDPAL/LCDNTSC/MULTI_TAITO/MULTI_PAL/MULTI_NTSC`
  × mono/dual × 16/32-bit × (wide-)interlaced (`PAL MONO 32 Bit
  Channel` … `Multi-Video NTSC 32 Bit Channel` format strings).
- Config dump strings: `----- PIX CONFIGURATION -----`,
  `Processor Memory Addr : 0x%lx` (= `procMem 0xD0000`),
  `Broadcast FIFO Addr : 0x%lx` (= `0x320`), `Video Card ioAddr :
  0x%lx` (= `0x340`), per-channel blocks (`Channel %2ld`, `Video Card
  Type : Standard/Multi`, `Video Card Mode : (0x%08lx)`),
  `Version (int)/(string)`, `Code Path : %s`.
- Monitor (same family as §12's `PIX 88110 Monitor`): `PixMon>`,
  `Quit - Quit monitor`, `SB addr - Select processor card`,
  `Could not find processor card at 0x%lX`, `No processor card
  selected`, `Processor A/B is still running`, `Illegal parameter`,
  `Not enough/Incorrect number of parameters`, `No processor base
  address set`, `Symbols already/Not loaded`, `Sending to channel %d`,
  `Unknown Standard 0x%04lx`, `v%02ld.%02ld.%02ld`, `Function : %s`.
- Errors: `PIX ERROR`, `Critical PIX Error`, `PIX Assertion failed :
  File %s, line %ld`, `PIX Fatal Error ..`, `Fatal Error in PIX Memory
  Table`, `ERROR : Unknown texture type 0x%08lx`, `VideoSync structure
  error`, `Could not allocate memory for blit`, `Cannot update a
  standard model data structure`.
- Asset refs: `models\player\ehand.bin`, `models\player\gun2.bin`
  (hand/gun sensor meshes), `dactyl.vst`, `newblits\*.vst`
  (gameover/presstr/pressb/pressttc/hiscore/egg1-11).

## 7. Sound usage (dual Soundscape)

- `snd.log` open/close, `Opening Sound Library Version %s`,
  `SND_Open call returned %d`, `SND Error : (%d) %s`.
- Firmware download flow: `Found and opened sound kernal` [sic],
  `Sent Sample Download Request`, `Sample %d Card %d Size %d`,
  `Sample Download request Failed!`, `Download Sample Failed`,
  `Sample, Patch and Program Installed, sample duration %d`,
  `Sample install failed during patch/program download`.
- Dual cards: `sound1`/`sound2`, `Configure SoundScape Card1/2
  failed...`, `Failed to configure card 1`, `Getting/Got/Set Hardware
  Config`, `%d Cards found in config.vpc`, `dmadrq`/`dmadack`,
  `Assigning PC DMA Channels`, `Unable to open DMA Facilities`,
  `DMA Opened and initalised`, `DMA Timed out`, `Invalid PC DMA
  Channel - 0,1,3,5,6,7 are valid !`, `DMA Buffer 0x%02x:0x%08x`,
  `MIDI Emulation` ack/channels.
- `RUNFILES\VOLUMES.DN2` (dactyl-specific mix):
  `STEPVOL/HITPLAYERVOL/FALLVOL/DACVOL/BULLVOL/HUNGRYVOL/COMEVOL/
  HYPERVOL/PHAZEVOL` at 100, `HITOBJECTVOL` 80, `INSTRVOL/SELECTVOL/
  TTDVOL/SFXVOL/GURUVOL` 127. In-game changers: `Set/full/half/zero
  volume`, `Change volume (+/- 1/10)`, `Save/Load volumes`, `Mix Level`
  table, ADSR `Attack/Decay/Sustain/Release/Start/End Level`.

## 8. Network usage (`DAC2` magic)

- `net.log` open/close, `Opening Network Library Version %s`,
  `NET Error : (%d) %s`, `Network card at 0x%03x`,
  `Network IO/Mem address`, `Network IRQ %d`, `No [NET] section /
  Error in the config.vpc [NET] section`, `io address/mem
  address/IRQ number in config file is invalid !`,
  `No configuration File Default settings assumed`.
- NE2000-class programming strings: `Setting up the card`,
  `Port/Irq/RamBase/RamSize/RamUsable` (`Port 0x%x, Irq 0x%x...`),
  `(port >0x279) && (port < 0x2A0)` (probe-range guard covering
  `0x27A–0x29F`), `Board id mask`, `Receive Start`,
  `Ring info: Base/ReceiveStart/ReceiveStop`, `Start/Last Rcv Ring`,
  `Set receive mask`, `Overflow condition !`,
  `Boundary updated to 0x%x because of bad NextPointer`,
  multicast add/register strings, `Not processing packets - No Card
  present`, `Packet already full`, `No Network Card(Config found)`,
  `Network card NOT reset`, `Network Card IO Memory Bad`,
  `The NET library says - No, wrong HPP version, try again`,
  `Clearing : Network packet data`, `Network Packet size = %d`,
  `F = %.2f C = %.4f Packet = %d`, `c\netlib.cpp` (source path leak).
- Game net strings: `DAC2` (×3 slots), `Error Network Fault  !!`,
  `Initialising 2-Player game`, `1000000 - 1 : Caught 2Player, 4Player
  lock`, `Error! Bad difficulty level` — `DAC2` is DN2's wire magic
  (cf. `dumps_network.html` 131 B frames to `01:44:41:43:32:00`).

## 9. CD + config + DOS surface

- Dual CD path (same as ETS): `Drive %d is an Mscdex Drive` /
  `is a Mitsumi Drive at 0x%x` / `is a MITSUMI on port %x` /
  `is set to MSCDEX`, `Int 0x2F call failed - MSCDEX Not found`,
  `MSCDEX is NOT Avalible !`, `unable to allocate MSCDEX
  control/workspace`, `Playing Mscdex Drive %d, Priority %d`,
  `Resetting Drive %d, Mitsumi Drive on port 0x%0x`.
- Config: `c:\config.vpc` (`O@VPCCONFIGFILE`), `c:\keyword.vpc`,
  `vpclog`, `vpctemp`/`set vpcexit/vpcerror/vpcexp`, `No configuration
  File Default settings assumed`, `Could not open/Cannot open config
  file`, `Illegal token/value`, `Configuration structure invalid`,
  `SU2000`, `Illegal console type/config update`,
  `Unable to locate config.vpc`, `Bad Configuration File`,
  `Keyword %s found but NOT recognised`,
  `%d Cards found in config.vpc`; `Error during/after FPGA config`,
  `Insufficent data in config file` (VID1000 programming path).
- Env vars consumed: `DOS4GPATH`, `dos4gw.exe`, tracker `trkdelay`/
  `trkreset`, `Spacepad`, `VPCCONFIGFILE`.
- Allocator traces: `real alloc of 0x%x returned Selector 0x%x,
  Segment 0x%x`, `Allocated Selector 0x%02x`, `unable to map start of
  segment/limit`, `DMA Workspace Alloction`, `Window : %ld, Address :
  0x%08lX` + min/max X/Y (viewport setup).
- Game shell: `Dactyl Nightmare 2 - The Race For The Eggs`,
  `Games played = %d`, `Player %ld`, `Change player`, `Select
  signal/max/min`, `center X/Y : %ld` (×2 sites: HMD + monitor?),
  `AUTO_SELECT`, `Entering Spacepad Open`, `Sorry…` menus implied by
  `presstr/pressb/pressttc` newblits.

## 10. What DN2 needs from emulation (read against `SU2000.md`)

1. **TRK**: same `W`/`S` ISA protocol as ETS but lib 01.04.42 with a
   firmware-version gate (`TRK BAD VERSION`) + 2000 ms startup delay +
   metal compensation + sensor offsets — the strictest consumer; the
   sign-on must satisfy the version parse, not just the substring.
2. **CTRL/FORMAT**: version/status/sync/joystick map (§5) + credit +
   backlights + 1P/2P link modes; `Format C/D not found` must degrade
   gracefully (DN2 prints and continues like ETS).
3. **PIX**: dual proc cards (`procMem 0xD0000`, procs `0x300/0x360`,
   BCST `0x320`, VID `0x340 WIDEPAL_DUAL_32` per `CONFIG1/2.VPC`);
   per-game PIX firmware is NOT in the dumps (loaded from CD at the
   time) — headless testing stops at `Could not find processor card`.
4. **SND**: two Soundscape cards (`0x330/IRQ12` + `0x350/IRQ7`,
   `SNDSCAPE.COD` download, DMA 1,3) — unemulated; must fail the way
   real missing cards do (`Configure SoundScape Card failed`).
5. **NET**: NE2000-class at `0x280/C8000/IRQ5` speaking `DAC2`
   (NOT `ZONEH`); `No Network Card` must degrade to solo play.
6. **CD**: MSCDEX+Mitsumi dual path; `*.DBS/*.TEX/*.WAV/*.VIC` +
   `RUNFILES` content pipeline per §12.

## 11. Claim → source map

- Dir/file inventory → `V2k\...\DN2\` listings (Simon's + Faulty's).
- Game names/modes → `DN2\DN2.INF`, `DN2RUN.BAT`, `RNT`.
- Binary identity → `rz-bin -I/-S/-e` + entry `pd` on Simon's `DN2.EXE`.
- All behavior strings → raw-ASCII scan of Simon's `DN2.EXE`
  (file offsets `0x…` in §§4–9; 2459 runs ≥6).
- Version drift → `$VER/$DATETIME` in Simon's `DN2/DN2C.EXE` vs
  Faulty's `DN2.EXE`.
- `DAC2` wire magic → strings at `0xd76e0` + `dumps_network.html`.
- RUNFILES content → Faulty's `DN2\RUNFILES\` (quoted verbatim:
  `DEFAULTS/VOLUMES/HISCORES/LOOK`).
- Linker segments → Faulty's `MAINA.OUT` head (`.vecs/.texture/
  .bitmap/.font/.fmvmem/.pmem`).
- Opcode census → raw byte counts on Simon's `DN2.EXE` (heuristic).
