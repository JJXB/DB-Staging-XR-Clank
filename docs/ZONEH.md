# Zone Hunter — Technical Infodump

Source trees surveyed (both dumps carry a `ZONE` install; they are
WIDELY different builds, see §3):
- `D:\Emu\DOS\C\V2k\VR 2000 Series Disc Image (Simon's)\ZONE\`
  (`ZONE.EXE` 765072 B, `ZONE.INF`, 9× `*RUN.BAT`, `ZONERUN.BAT`,
  `NETTEST.BAT`, `CPIC.BAT`, `EXPEXIT.BAT`, `MAINA.OUT`, `MAINB.OUT`,
  `SNDSCAPE.COD`, `HISCORE.DAT`, `ZONE.MAP`, `ZONE.SMP`)
- `D:\Emu\DOS\C\V2k\VR2000 HDD Dump (Faulty's)\ZONE\`
  (`ZONE.EXE` 759988 B, `ZONE.INF`, same bats + `LOG.TXT`,
  `NETTEST.EXP` 65081 B, `SND.LOG` 38125 B, `TRKDEBUG` 0 B, `ZONE.MAP`,
  `ZONE.SMP`, `SNDSCAPE.COD`, `HISCORE.DAT`, content dirs `ANIM/
  BITMAPS/DATA/DOCS/FONTS/MODELS/RUNFILES/SOUNDS/TEXTURES`)

Method: `rz-bin -I/-S/-e/-zq` for structure, raw-ASCII scan
(2279 printable runs ≥6 in Simon's `ZONE.EXE`) for behavior, bounded
entry disassembly, opcode/INT byte census (heuristic — opcodes also
occur as data, same caveat as `SU2000.md` §11). Full linear disassembly
infeasible at this size. Nothing below comes from any emulator source.

## 1. The 30-second version

1. Zone Hunter is the flagship 2-player VR shooter (ninjas, turrets,
   weapons, rooms, eggs 1–11 newblits): 9 modes (solo/FMV/camera/network
   combos) via `ZONE.INF` + `*RUN.BAT` (`zone /s /c /T /D3 ...` flags).
2. The two dumps are a year apart in spirit: Simon's `ZONE.EXE` is
   experience **v11.03.02** (TRK 01.04.39, CTRL 01.03.45, early-1995
   libs); Faulty's is **v00.06.00** (TRK 01.04.00, CTRL 01.03.25, VPC
   lib, NET v02.09.00) — an early development build.
3. Network magic is `ZONEH` (3× slots + `recieve`/`send` atom table),
   matching the `01:5A:4F:4E:45:48` 60 B frames in `dumps_network.html`;
   ZONE never says `DAC2`. `NETTEST.EXP` (65 KB) is a network-test
   experience/script.
4. Faulty's `SND.LOG` is a REAL boot log from hardware: 2 Soundscapes
   (`0x330/IRQ12` + `0x350/IRQ7`), DMA workspace (`Selector 0x1c8`),
   kernel load — the exact sequence our emulation must one day survive.
5. PIX side is dual-88110 with channel/procmem/BroadcastFIFO config
   prints; content pipeline is `MODELS/ (NINJA/RECEP/ROOMOBJ/ROOMS/
   TURRET/WEAPONS .BIN)` + `TEXTURES/ (*.TEX)` + `SOUNDS/ (*.WAV)` +
   `RUNFILES/ (*.DBS/VICS)` + `ZONE.TSC`.

## 2. Install layout + launch chain

- `ZONE.INF` (428 B, identical both dumps): 9 `CODE`/`NAME` pairs —
  `ZHS` solo NO FMV / `ZFMV`+`ZSFMV` with-FMV / `ZC`/`ZCFMV` camera /
  `ZHN`/`ZHSN` with-NETWORK / `ZFN`/`ZSFN` FMV variants. Run bats:
  `ZCRUN/ZCFMVRUN/ZFMVRUN/ZFNRUN/ZHNRUN/ZHSN/ZHSRUN/ZSFMVRUN`
  (e.g. `ZHSRUN.BAT` = `zone /s /c /T /D3`; `ZONERUN.BAT` =
  `zone /e /c /T /D3 /1 /vcc` per §10: `/e` experience, `/c` camera?,
  `/T` tracker?, `/D3` channel?, `/1` player 1).
- `NETTEST.BAT` = `run386 nettest` (32-bit DOS extender test runner?)
  + `NETTEST.EXP` 65081 B; `CPIC.BAT` 1684 B (palette/picture
  converter batch?); `EXPEXIT.BAT`.
- `SNDSCAPE.COD` 61440 B both (shared firmware blob).
- `ZONE.MAP` 122155 B (linker map) + `ZONE.SMP` 16815 B + `MAINA.OUT`/
  `MAINB.OUT` (build logs) — toolchain droppings, useful for symbol
  hunts later.
- `HISCORE.DAT` 362 B both (hiscore table).
- Flag files: Faulty's `TRKDEBUG` 0 B (tracker-debug enabler, cf.
  ETS `TRKLOG`/`ETSDEBUG`) + `LOG.TXT` 0 B.
- Faulty's content dirs (see `SU2000.md` §12 for the pipeline):
  `RUNFILES\` (`ZONE.TSC`, `VOLUMES`, `*.DBS`, `VICS\TRAIN.VIC`
  10 MB), `MODELS\` (`NINJA/RECEP/ROOMOBJ/ROOMS/TURRET/WEAPONS`
  hundreds of `.BIN` + `CHKLIST.MS`), `TEXTURES\` (`*.TEX` +
  `TEX16BIT\`), `SOUNDS\` (hundreds of `.WAV` + `JAPAN\` dub).

## 3. Binary identity + build drift

- MZ + `LE`, Watcom 10.0a + Rational DOS/4GW, 80386, static. Code
  object 1: 0x7F000 B; entry `0x58960` = `jmp` stub (same shape as DN2's
  `0x68420`). Same DOS4GW stub family (`dos4gw.exe`/`DOS4GPATH`, WATCOM
  banner at the same file offsets as DN2).
- Opcode census on Simon's (heuristic): `EC` 3060 / `E4` 1189 /
  `EE` 833 / `ED` 555 / `EF` 396 / `E6` 526 — same heavy port-IO
  profile as DN2; INT census DOS `0x21` 110 + video `0x10` 76 on top,
  rest noise.
- Version matrix (Simon's v11.03.02 vs Faulty's v00.06.00):

  | Lib | Simon's (1995) | Faulty's (early/dev) |
  |---|---|---|
  | ZONE experience | v11.03.02 | v00.06.00 |
  | EXP | 01.02.24 (Feb 22 95) | 01.02.09 |
  | VPC lib | — | v00.00.28 |
  | PIX (monitor) | May 02 95 | — |
  | TRK | 01.04.39 (Sep 08 95) | 01.04.00 |
  | CTRL | 01.03.45 (Mar 24 95) | 01.03.25 |
  | CD | 00.01.34 | 01.09.03 (!) |
  | MISC | 00.01.06 (Feb 19 95) | — |
  | SND | 00.01.56 | 00.01.26 |
  | NET | Jul 06 95 / lib v00.02.29 | v02.09.00 (!) |
  | STAT | 00.01.13 | 00.01.02 |
  | VEL | 01.00.00 (Feb 13 95) | — |
  | PIX lib | v01.05.61 | v01.03.60 |

  (Faulty's CD `01.09.03` and NET `02.09.00` vs Simon's `00.01.34`/
  `00.02.29` are different numbering generations — dev-branch libs.)

## 4. Tracker usage (TRK 01.04.39 — between ETS and DN2)

- Same `W`/`S` open flow (`Trying to open card %ld as an InsideTRAK`,
  `InsideTRAK string not found` ×2, `Opening tracker library v%s`,
  `Tracker Library Open complete`, `Cannot open tracker`,
  `Tracker not open/responding`, `Sensor not active`,
  `Illegal tracker/sensor`, `TRK ERROR`).
- Has DN2-era strictness (`TRK BAD VERSION`, `trkdelay` + 2000 ms
  default, `trkreset`, sensor offset/world offset/frequency/metal
  compensation, `Set sensor use to %x`) plus one DN2 lacks:
  `trkframe` / `Tracker Reference Frame required`.
- ZONE-only: `Infra red tracking is not possible (see MAIN.c)` +
  `Infra Red Tracker :` (abandoned IR-tracker experiment, pointer to
  source file!), `Serial track not found` (serial fallback),
  `Reading : CD track data` (disc-track reuse of the word).
- Same `KEYWORD.VPC` grammar (`TRACKER/TRACKER%ld/SENSOR%ld/FASTRAK/
  INSIDETRAK/NO_TRACKER/RANDOM_TRACKER1/FIXED_TRACKER/SENSOR_OFFSET/
  HEMISPHERE/SENSOR_MAP/SENSOR_DATA/CONSOLE_TYPE`), `trk.log`.

## 5. Control / FORMAT usage

- `Cannot open CTRL library/format card`, `CTRL library/hardware not
  open/responding`, `Illegal joystick/format`, `Format C/D not found`,
  `Backlight disabled`, `CTRL ERROR`.
- ZONE-only: `Joystick connected through format board`,
  `CTRL visette error %d` (×10 sites — per-frame visette health),
  `PROCESS_1/2 : number of CTRL cards is 0, update CONFIG.VPC`,
  `PROCESS : Setting visette brightnes [sic] to 150/255 for PLR_1/2`
  (150 menu / 255 game brightness!), `credit toggle (default ON)`,
  `credits`, `continual credit`, keyboard/credit-device strings.
- Same `[CTRL]/FORMAT_CARD/NO_FORMAT`, `MICNET/VISETTE/VISETTE_MASTER`
  volume namespace.

## 6. PIX / video usage

- Same 32-mode enum + config dump (`Processor Memory Addr`,
  `Broadcast FIFO Addr`, `Video Card ioAddr/Mode`, channel blocks,
  `Version (int)/(string)`, `Code Path`) and monitor family
  (`PixMon>` implied by `Quit - Quit monitor`, `SB addr - Select
  processor card`, `Could not find/No processor card...`,
  `Processor A/B is still running`, `Sending to channel %d`).
- Asset refs: `runfiles\zone.tsc`/`zone16.tsc` (+ `Cant open`
  variants), `Rooms\room%d.bin`, `runfiles\soundF.dbs` (+ `Cant
  open`), `center X/Y : %ld` (×2: HMD + monitor),
  `Reading : Room/Room object list/Camera/Hi Score data`,
  `Unknown texture type`, `VideoSync structure error`.
- Game shell: `Game Designers`, `One/Two player game`,
  `1/2 player game selected`, `Player start point = %d`,
  `Camera machine`, `Camera`, `Play Game`, `Player %d is now
  STARTING/ENDING game`, `Illegal game mode %d`,
  `Put game into soak test`, `Do use game timer`,
  `DONT check players hits`, `Options selected :`,
  `$COP:(c) Virtuality Entertainment Limited 1993, 1994 and 1995`.

## 7. Sound usage (matches `SND.LOG` — real hardware trace)

- Same dual-card download flow as DN2 (`snd.log`, `sound1/sound2`,
  `SNDSCAPE.COD`, DMA channels/modes/buffers, `SND Error`).
- Faulty's `SND.LOG` (38 KB) is a genuine boot trace — anchor values:
  `Opening Sound Library Version 00.01.56`, `2 Cards found in
  config.vpc`, `Card 1 at address 0x330 using Interrupt 12`,
  `Card 1 at address 0x350 using Interrupt 7` (sic — second card
  mislabeled "Card 1" in the print), `real alloc of 0x10000 returned
  Selector 0x1c8, Segment 0x1c2d`, `DMA Buffer 0x158:0x00020000`,
  `DMA Buffer Size 0xc2d0 (48k)`, `DMA Opened and initalised`,
  `Loading code using channel 3`, `Found and opened sound kernal`,
  `Loading Kernel`, `03:0xc2d0.....`.
- ZONE-only: `Sound/Music`, `Number of sample Effects`,
  `Loading Sounds`, `ERR - SND was not open error (%d)`.

## 8. Network usage (`ZONEH` magic + atom table)

- Same NE2000-class base (`net.log`, `Network card at 0x%03x`,
  IO/Mem/IRQ prints + validity errors, `No [NET] section`,
  ring/multicast/packet strings, `The NET library says - No, wrong
  HPP version, try again`).
- ZONE-only packet layer: `--> No packets to process`,
  `Packet start 0x%x:0x%x`, `received %d packets`,
  `Sent to/from : %02x...`, `Payload = %d bytes`,
  `Atom Type %d, length %d`, `Calling Function @ 0x%x:0x%x`,
  `Current restarts at 0x%x, Boundary at 0x%x`,
  `Interrupt Routine called 0x%02x times`, `No Ack on control
  value/Tuning/Setting Synth Gate`, `Output 0x%x to 0x%x` (×4 —
  direct port-IO trace points!), `Invalid revision Number`,
  `Network ON`, `Clearing : Network packet data`.
- Wire magic at `0xa9db0`: `ZONEH\0ZONEH\0ZONEH\0` + `recieve\0`
  [sic] + `send\0` + doubles — a send/receive atom table; the
  `01:5A:4F:4E:45:48` frames in `dumps_network.html` are its
  on-wire form. ZONE never says `DAC2`.

## 9. CD + config + DOS surface

- Same dual path (`Mscdex`/`MITSUMI on port`, `Int 0x2F call
  failed`, `Playing Mscdex Drive %d, Priority %d`).
- Same `c:\config.vpc` + `c:\keyword.vpc` + `vpclog`/`vpctemp` +
  validity errors + `SU2000` + `Illegal console type`; `FPGA
  config` errors; env (`DOS4GPATH`, `Spacepad`, tracker vars);
  allocator traces (`real alloc ... Selector/Segment`,
  `Allocated Selector`, `unable to map segment...`,
  `Viewport Window/Address/min/max`).

## 10. What ZONE needs from emulation (read against `SU2000.md`)

1. **TRK** 01.04.39: `W`/`S` + version gate (`BAD VERSION`) + 2000 ms
   delay + offsets/compensation — between ETS and DN2 in strictness;
   same sign-on contract as §8.
2. **CTRL/FORMAT**: §5 map + credits + visette errors/brightness
   (150/255!) + 1P/2P card counts; missing cards degrade to menus.
3. **PIX**: dual proc (`0x300/0x360`, `procMem`, BCST `0x320`, VID
   `0x340`); per-game PIX firmware from CD; `Could not find
   processor card` is the headless stop point.
4. **SND**: two Soundscapes exactly per `SND.LOG`
   (`0x330/IRQ12` + `0x350/IRQ7`, DMA, kernel download) — the log is
   the conformance script.
5. **NET**: NE2000 `0x280/C8000/IRQ5` speaking `ZONEH` atoms
   (send/recieve table); solo play on `No Network Card`.
6. **CD**: MSCDEX+Mitsumi; content pipeline per §12
   (`.BIN` models + `.TEX` + `.WAV` + `.VIC` + `.TSC/.DBS`).

## 11. Claim → source map

- Dir/file inventory → `V2k\...\ZONE\` listings (Simon's + Faulty's).
- Game names/modes/flags → `ZONE.INF` + `*RUN.BAT` + `NETTEST.BAT`.
- Binary identity → `rz-bin -I/-S/-e` + entry `pd` on Simon's
  `ZONE.EXE` (same LE/Watcom shape as DN2).
- All behavior strings → raw-ASCII scan of Simon's `ZONE.EXE`
  (file offsets `0x…` in §§4–9; 2279 runs ≥6).
- Version drift → `$VER/$DATETIME` in Simon's (`v11.03.02`) vs
  Faulty's (`v00.06.00`) `ZONE.EXE`.
- `ZONEH` wire magic → atom table at `0xa9db0` +
  `dumps_network.html`.
- Ground-truth sound boot → Faulty's `ZONE\SND.LOG` (quoted §7).
- Linker segments → `MAINA.OUT`/`ZONE.MAP` (same custom
  `.texture/.bitmap/.font/.fmvmem` family as DN2).
