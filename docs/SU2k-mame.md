# Virtuality SU2000 - MAME Driver Infodump

Source tree surveyed: `C:\Users\jjxb_\GIT\mame` at
`mame0289-1638-g7e7a99fd7b5` (HEAD 2026-10-18). Primary file:
`src/mame/misc/su2000.cpp` (302 lines, BSD-3-Clause, copyright
Philip Bennett). Supporting sources read for context:
`src/mame/shared/pcshare.{h,cpp}`, `src/devices/video/pc_vga.*`,
`src/devices/cpu/i386/i386.*`, `src/devices/cpu/m88000/*`,
`src/devices/cpu/tms320c3x/*`, `src/devices/machine/ds128x.*`,
`src/devices/machine/mc146818.*`, `src/emu/{romentry,romload,addrmap,screen,xtal}.*`,
`src/mame/mame.lst`.

Scope: **what the MAME driver actually contains and does**, line by line -
device tree, address maps, clocks, ROM set, and the gap between it and the
real hardware. This is an independent reading of the emulator source; the
hardware facts it is compared against live in `docs/SU2000.md` (arianchen.de
mirror, `V2k` dumps, SkullSpace notes), not here. Nothing below is copied
from that document.

## 1. The 30-second version

1. `su2000` is a **preliminary, non-booting** MAME machine. It is declared
   `MACHINE_IS_BIOS_ROOT | MACHINE_NOT_WORKING | MACHINE_NO_SOUND`
   (`src/mame/misc/su2000.cpp:302`) and its own TODO list still begins with
   *"Get system booting"* (`:36`).
2. Only the **host PC side** is instantiated: an Intel 486DX-33, the generic
   `pcat_base_state` southbridge set (two 8259s, two 8237s, an 8254, an
   8042, an AT keyboard), a DS12885 RTC, and a plain VGA with 1 MB VRAM.
3. None of the custom boards are emulated. The PIX 1000, VID 1000, FORMAT C,
   FORMAT D, TRACKER and NETWORK cards exist only as **commented-out device
   creation** (`:154-166`) plus ROM data in the ROM set.
4. There is **no storage**: no IDE controller, no HDD, no CD-ROM, no floppy,
   no serial, no parallel, no network, no sound device. The `idectrl.h`
   include (`:56`) and the `ide_interrupt` stub (`:132-137`) are dead code.
5. The ROM set is mostly **PLD/PAL dumps**, not firmware the emulated CPUs
   can run. The two 486 BIOS images are the only code the driver can execute.
6. As a result the machine reaches BIOS POST (or fails there) and has
   nothing to boot from; the driver is effectively a hardware inventory with
   a PC skeleton around it.

## 2. Identity and history

| Field | Value | Source |
|---|---|---|
| MAME short name | `su2000` | `mame.lst:33607` |
| Source file | `src/mame/misc/su2000.cpp` | `mame.lst:33606` |
| Display name | `SU2000` | `:302` |
| Manufacturer | `Virtuality` | `:302` |
| Year | `1993` | `:302` |
| Parent / clone | none (`0`), BIOS root | `:302` |
| State class | `su2000_state : public pcat_base_state` | `:81` |
| Init | `empty_init` | `:302` |
| Rotation | `ROT0` | `:302` |
| Sound flag | `MACHINE_NO_SOUND` (sound exists on real h/w, not emulated) | `:302` |
| Save state flag | absent (no `MACHINE_SUPPORTS_SAVE`) | `:302` |

History from `git log --follow`:

| Date | Commit | Change |
|---|---|---|
| 2012-01-05 | `2bec585837c` | Driver added as NOT_WORKING "Virtuality SU2000 [Phil Bennett, SailorSat]" |
| 2016-03-21 | `d3a18f29af2` | "fix cmos size, nothing to boot from though" - 4 MB bank RAM replaced by straight `.ram()` maps, DS12885 added |
| 2022-06-06 | `9b21cf3b566` | Header notes expanded: SU2000 vs SD2000, game lists, technical-manual link (#9672) |
| 2022-2026 | refactors | Source moved to `misc/`, `screen_device` split, device clock plumbing, TMS320 renaming - no functional SU2000 work |

The header lists known software (`:9-22`):

- SU2000 (stand-up): Boxing, Dactyl Nightmare SP, Dactyl Nightmare 2 -
  Race For The Eggs!, PacManVR, Shoot For Loot, Sphere, Zone Hunter.
- SD2000 (sit-down): Buggy Ball, Ghost Train, Missile Command VR,
  X-Treme Strike.

The header states SU2000 and SD2000 share the base hardware: 486DX-33,
8 MB RAM, and a customised "Expality PIX 1000" card with 16 MB video RAM
carrying **dual Motorola MC88110** CPUs, with per-game firmware for both
(`:28-33`). Note the spelling "Expality" in the source; the hardware
documentation in `docs/SU2000.md` calls the board `PIX 1000` (WI01REVB).
Only one machine entry exists - no `sd2000` driver or clone, and no game
sets are declared.

## 3. What is actually instantiated

`su2000_state::su2000(machine_config &config)` (`:146-183`):

| Device | Tag | Type / clock | Notes |
|---|---|---|---|
| Main CPU | `maincpu` | `I486` @ 33 MHz (`I486_CLOCK`) | `:149`; 32-bit program and I/O spaces |
| IRQ acknowledge | - | `pic8259_1::inta_cb` | `:152` |
| Video out | `screen` | raw `25.1748_MHz_XTAL`, 900x526 total, 640x480 visible | `:170-172`; ~53.18 Hz |
| VGA | `vga` | `VGA` with 1 MB VRAM | `:174-176`; control-terminal output only |
| PC chipset | - | `pcat_common(config)` | `:178` |
| RTC | `rtc` (replaces `m_mc146818`) | `DS12885` | `:180-182` |

`pcat_common()` (`src/mame/shared/pcshare.cpp:182-190`) pulls in
`pcat_common_nokeyboard()` (`:142-180`) and an AT keyboard:

| Device | Tag | Configuration |
|---|---|---|
| Master PIC | `pic8259_1` | IRQ out -> CPU line 0; slave-ack callback; `in_sp` = 1 |
| Slave PIC | `pic8259_2` | IRQ out -> master IR2; `in_sp` = 0 |
| DMA 1 | `dma8237_1` | 14.318181 MHz / 3; HRQ -> CPU HALT/HLDA; page callbacks |
| DMA 2 | `dma8237_2` | 14.318181 MHz / 3 |
| PIT | `pit8254` | ch0 4.772720/4 MHz -> PIC1 IR0 (heartbeat); ch1 DRAM refresh; ch2 speaker gate via KBDC OUT2 |
| RTC (pre-replace) | `rtc` | `MC146818` @ 32.768 kHz, IRQ -> slave PIC IR0 |
| KBDC | `kbdc` | `KBDC8042_STANDARD`; reset -> CPU RESET; A20 -> CPU A20; IBF -> PIC1 IR1 |
| Keyboard | `at_keyboard` | `AT_KEYB`, type AT, scan-code set 1 |

The DS12885 replacement (`:180-182`) is what the 2016 commit was about: the
generic `MC146818` has 64 bytes of NVRAM while the DS12885 has 128
(`src/devices/machine/ds128x.h:14`), and the driver re-attaches the IRQ to
`pic8259_2` IR0 and sets the century index to `0x32` (BCD century at RTC
register 0x32) - the same values `pcat_common` used.

Important omissions in the machine config:

- No `ide_controller_device`, `ata_interface`, `harddisk_image_device`,
  `cdrom_image_device`, or `floppy_image_device`.
- No `dp8390d_device` (network), no `es5506_device`/Ensoniq, no MCS-96 or
  other FORMAT C MCU, no `TMS320C31` tracker DSP, no `MC88100`/MC88110.
- No `INPUT_PORTS` definition at all (`:119-123` is an empty section) and the
  GAME entry passes `0` for ports; input exists only through the AT keyboard
  device's own default ports.
- No `MACHINE_NO_COCKTAIL`/layout artwork.

## 4. Address maps

### 4.1 Program space (32-bit, `AS_PROGRAM`)

`su2000_state::pcat_map` (`:102-110`):

| Start | End | Mapping | Backing |
|---|---|---|---|
| `0x00000000` | `0x0009ffff` | `.ram()` | 640 KB conventional |
| `0x000a0000` | `0x000bffff` | `.rw(vga::mem_r, vga::mem_w)` | 128 KB VGA window |
| `0x000c0000` | `0x000c7fff` | `.rom()` | `maincpu` region offset `0xc0000` = VGA BIOS (32 KB) |
| `0x000c8000` | `0x000effff` | *unmapped* | gap |
| `0x000f0000` | `0x000fffff` | `.rom()` | `maincpu` region offset `0xf0000` = system BIOS (64 KB) |
| `0x00100000` | `0x003fffff` | `.ram()` | 3 MB extended |
| `0xffff0000` | `0xffffffff` | `.rom().region("maincpu", 0x0f0000)` | 64 KB BIOS alias at top of 4 GB |

Details worth knowing:

- `.rom()` with no region argument defaults to the CPU's own tag (`maincpu`)
  and to a region offset equal to the map start, for `AS_PROGRAM` entries
  (`src/emu/addrmap.cpp:1152-1156`). So the two `rom()` entries are
  self-consistent with the ROM layout.
- The 486 reset vector is `CS.base = 0xffff0000`, `EIP = 0xfff0`
  (`src/devices/cpu/i386/i386.cpp:2958-2975`), which lands in the top alias
  and therefore executes the AMIBIOS image.
- Total mapped RAM is **0x0a0000 + 0x300000 = 3.625 MB**, not the 8 MB stated
  in the header (`:30`). Addresses `0x00400000`-`0xfffeffff` are unmapped.
- The VGA memory window is only `0xa0000-0xbffff`; the 1 MB VRAM is not
  linearly mappable by the guest through this map.
- There is no PCI/PCIe configuration mechanism, no chipset/ALI M1429
  emulation, and no SMM/APM hooks beyond the i386 core defaults.

### 4.2 I/O space (32-bit, `AS_IO`)

`su2000_state::pcat_io` (`:112-116`):

| Start | End | Mapping |
|---|---|---|
| `0x0000` | `0x00df` | `pcat32_io_common(map)` (`src/mame/shared/pcshare.cpp:122-140`) |
| `0x03b0` | `0x03df` | `vga::io_map` submaps |

`pcat32_io_common` covers the standard AT decode:

| Ports | Device |
|---|---|
| `0x000-0x01f` | `dma8237_1` |
| `0x020-0x03f` | `pic8259_1` |
| `0x040-0x05f` | `pit8254` |
| `0x060-0x06f` | `kbdc` data/status |
| `0x070-0x07f` | RTC address/data (split low/high byte masks) |
| `0x080-0x09f` | DMA page select registers |
| `0x0a0-0x0bf` | `pic8259_2` |
| `0x0c0-0x0df` | `dma8237_2` |

`vga::io_map` (`src/devices/video/pc_vga.cpp:275-313`) maps the VGA's
internal `0x00-0x2f` window, which through the `0x3b0` base covers
`0x3b0-0x3df` (Misc/CRTC/Sequencer/ATC/GC/RAMDAC and the 3Bx/3Dx alias
switch). Ports used by the real SU2000 custom cards - e.g. TRACKER at
`0x270`, PIX at `0x2a0`/`0x300`/`0x360`, VID at `0x340`, BCST at `0x320`,
FORMAT C at `0x210`/`0xcc000` - are **not decoded by this driver**, so guest
accesses read open bus. (Compare `docs/SU2000.md` sections 4-8 and
`docs/su2k-ets.md` for the live port map.)

## 5. Clocks

Defines at `:69-72`:

| Define | Value | Used by |
|---|---|---|
| `I486_CLOCK` | `33000000` (33 MHz) | main CPU (`:149`) |
| `MC68000_CLOCK` | `XTAL(10'000'000)` | **unused** |
| `TMS320C1_CLOCK` | `XTAL(33'833'000)` | only the disabled tracker block (`:155`) |
| `MC88110_CLOCK` | `XTAL(40'000'000)` | only the disabled PIX block (`:158`, `:161`) |

`33.833 MHz` is a real catalogue crystal (`src/emu/xtal.cpp:453`,
`33.833_MHz_XTAL`). The TRACKER oscillator in the hardware documentation is
`33.3333 MHz` (`docs/SU2000.md` section 8), so the driver's tracker clock
does not match the documented board. The `40 MHz` PIX clock does match
`docs/SU2000.md` section 4b. `MC68000_CLOCK` matching the FORMAT C board's
10 MHz oscillator is coincidental: the FORMAT C controller is an N80C198
(MCS-96), not a 68000, and the define is never referenced.

## 6. Video

`:168-176`:

```
screen_device &screen(SCREEN(config, "screen"));
screen.set_raw(25.1748_MHz_XTAL, 900, 0, 640, 526, 0, 480);
screen.set_screen_update("vga", FUNC(vga_device::screen_update));

vga_device &vga(VGA(config, "vga"));
vga.set_screen("screen");
vga.set_vram_size(0x100000);
```

- `set_raw(xtal, htotal, hbend, hbstart, vtotal, vbend, vbstart)` gives a
  640x480 visible area in a 900x526 raster at 25.1748 MHz, i.e.
  `25,174,800 / (900 * 526) = 53.18 Hz`. This is the **VGA control
  terminal**, not the headset feed.
- `vga.set_vram_size(0x100000)` allocates 1 MB (`src/devices/video/pc_vga.cpp:137`).
  The documented Hualon SVGA board carries 256 KB (`docs/SU2000.md` section
  4a), so the driver over-allocates. The VGA BIOS is supplied as the
  `hmc86304_bios_v208.u7` ROM at `0xc0000`, matching the Hualon BIOS name.
- The comment at `:169` - `// TODO: Hualon ISA custom` - flags that the real
  video path (SVGA -> FORMAT C -> VID 1000 -> HMD) is absent; only a stock
  VGA device is present.
- `set_screen_update("vga", ...)` binds the stock `vga_device::screen_update`
  (`src/devices/video/pc_vga.cpp:1590`), which itself reconfigures the
  screen from CRTC registers on mode changes. The static `set_raw` values are
  therefore only the power-on/default geometry.

## 7. Disabled / aspirational hardware

`:154-166` is a single `#if 0` block:

```
tms320c31_device &tracker(TMS320C31(config, "tracker", TMS320C1_CLOCK));
tracker.set_addrmap(AS_PROGRAM, &su2000_state::tracker_map);

mc88100_device &pix_cpu1(MC88110(config, "pix_cpu1", MC88110_CLOCK));
pix_cpu1.set_addrmap(AS_PROGRAM, &su2000_state::pix_cpu_a);

mc88100_device &pix_cpu2(MC88110(config, "pix_cpu2", MC88110_CLOCK));
pix_cpu2.set_addrmap(AS_PROGRAM, &su2000_state::pix_cpu_b);

m68000_device &format_c(M68000(config, "format_c", XTAL(10'000'000)));
format_c.set_addrmap(AS_PROGRAM, &su2000_state::formatc_map);
```

This block **cannot be compiled as-is**:

- `MC88110` is not a device type. MAME has `MC88100` (`src/devices/cpu/m88000/m88000.h:91`)
  and an `mc88110_disassembler` (`m88000d.h:90`), but the core's TODO list
  still says `- mc88110` (`m88000.cpp:9`).
- `su2000_state::tracker_map`, `pix_cpu_a`, `pix_cpu_b` and `formatc_map`
  are **not defined anywhere** in the file (only the four `set_addrmap`
  references exist).
- `M68000` for FORMAT C is a placeholder: the real controller is an
  N80C198. MAME's MCS-96 family provides `i8xc196`/`i8x9x` (`src/devices/cpu/mcs96/`)
  but no 80C198 variant is instantiated.
- The PIX block would need two `MC88100` cores plus a shared-memory/mailbox
  model; the real PIX 1000 has dual MC88110s and per-game firmware, none of
  which is present in the ROM regions.

The header TODO list (`:35-44`) mirrors the same gaps: boot the system, add
game software, fix i386 protected mode, write the MC88110 core, emulate HDD
and CD, PIX 1000, VID 1000, tracker, and format cards.

## 8. ROM set

`ROM_START(su2000)` spans `:192-291`. There are **56 `ROM_LOAD` entries, 12
of them `NO_DUMP`**, so 44 files are expected to be dumped.

### 8.1 Regions

| Region | Size | Width/endian | Contents |
|---|---|---|---|
| `maincpu` | `0x100000` | default (8-bit) | VGA BIOS + system BIOS |
| `tracker` | `0x1000000` | 32-bit LE | TRACKER firmware, 2 x 128 KB interleaved |
| `format` | `0x8000` | default | FORMAT C U62 ROM (32 KB) |
| `pals` | `0x8000` | default | all PLD/PAL dumps for FORMAT/PIX/TRACKER/VID |

### 8.2 `maincpu`

| File | Offset | Size | CRC | SHA1 |
|---|---|---|---|---|
| `hmc86304_bios_v208.u7` | `0xc0000` | `0x8000` | `c6c32f1a` | `a07ade7f...` |
| `amibios_486dx_isa_bios.u32` | `0xf0000` | `0x10000` | `811d3639` | `a64d6026...` |

These are the only ROMs the emulated 486 can execute. Note the file names are
lower-case and the `.u7`/`.u32` suffixes are part of the names, unlike the
`V2k\SU2000 ROMS` dump tree which uses `HMC86304_BIOS_V208.U7` and
`AMIBIOS_486DX_ISA_...` names. There is no `sd2000` set and no per-game set.

### 8.3 `tracker`

| File | Offset | Size | CRC |
|---|---|---|---|
| `ver_151_03_u16.u16` | `0x00000` | `0x20000` | `8354d059` |
| `ver_151_03_u17.u17` | `0x00002` | `0x20000` | `ace4081d` |

`ROM_LOAD32_WORD` = `ROM_GROUPWORD | ROM_SKIP(2)` (`src/emu/romentry.h:149`):
each 2-byte group is written, then 2 bytes are skipped. With one file at
offset 0 and the other at offset 2, the result is 32-bit words whose low
half comes from U16 and high half from U17 - exactly the split described in
`docs/SU2000.md` section 8 (U16 = low half, U17 = high half), producing a
256 KB populated image inside a 16 MB region. Only VER.151.03 is included;
the VER.151.01/02/07 images mentioned in the hardware notes are absent.

Because the region is 16 MB (larger than the 4 MB auto-clear threshold in
`src/emu/romload.cpp:1430-1431`), the unpopulated 15.75 MB is **not
zero-filled** in release builds (only in `MAME_DEBUG`, where it is filled
with random data, `:1432-1434`). That is harmless today because no device
reads the region.

### 8.4 `format`

| File | Offset | Size | CRC |
|---|---|---|---|
| `wfc062_212.u62` | `0` | `0x8000` | `9a6b553a` |

One 32 KB ROM for the FORMAT C N80C198 (the hardware notes list WFC062 2.12
for FORMAT2). No MCS-96 CPU is attached, so it is loaded but never executed.

### 8.5 `pals`

All PLD dumps live in one `0x8000` region and **every entry uses offset
`0x00000`** (`:211-290`). They therefore overwrite each other; only the
last-loaded image survives. This is not a functional bug today (nothing
reads the region), but it means the region cannot be used as a lookup table
of the individual PLDs without splitting it into per-device regions or
distinct offsets.

Per-board inventory:

| Board | Type | Dumped | `NO_DUMP` | Notes |
|---|---|---|---|---|
| FORMAT | GAL20V8A | 0 | 1 | `wfc065a.u65` |
| FORMAT | PALCE20V8H | 0 | 2 | `wfc066a.u66`, `wfc067b.u67` |
| FORMAT | GAL16V8B | 0 | 3 | `wfc071a.u71`, `wfc014a.u14`, `wfc072a.u72` |
| PIX 1000 | PALCE16V8H | 14 | 0 | `wi01p006/026/044/045/046/050/053/054/056/058/067/075/079/080`, size `0x892` |
| PIX 1000 | PALCE20V8H | 8 | 0 | `wi01p048/049/070/072/073/074/083/092`, size `0xa92` |
| PIX 1000 | PALCE22V10 | 0 | 4 | `wi01p047/077/087/025` |
| TRACKER | TIBPAL16L8 | 0 | 2 | `u37 335d.u37`, `u40 4116.u40` (note the spaces in the names) |
| VID 1000 | PALCE20V8H | 8 | 0 | `wi02p000/001/002/003/004/005/006/007`, size `0xa92` |
| VID 1000 | PALCE16V8H | 9 | 0 | `wi02p008/009/010/011/012/013/014/015/016`, size `0x892` |
| **Total** | | **39** | **12** | |

Duplicate CRC groups in the set (expected for repeated PLD logic, but worth
knowing when auditing):

| CRC | Count | Files |
|---|---|---|
| `528142eb` | 2 | `wi01p006.u6`, `wi01p026.u26` |
| `bab34c4b` | 4 | `wi02p000.u23`, `wi02p002.u22`, `wi02p004.u6`, `wi02p006.u5` |
| `b438ce94` | 4 | `wi02p001.u32`, `wi02p003.u31`, `wi02p005.u14`, `wi02p007.u13` |

PLD file sizes `0x892` (2194) and `0xa92` (2706) are not powers of two and
are not the `0x100` placeholder used for undumped parts; they are raw
programmer dumps. `NO_DUMP` entries use `0x100` length placeholders.

There is **no ROM region for PIX firmware or VID firmware code**. The header
says each game ships independent firmware for both PIX CPUs (`:32`), but the
driver has no `pix` region, no per-game set, and no load address for it.
Likewise there is no network PROM (the hardware notes describe an
undumpable 74S288), and no CD/HDD image.

## 9. Known gaps vs. the real machine

Cross-referenced against `docs/SU2000.md`; all emulator claims below are
from `src/mame/misc/su2000.cpp`.

| Subsystem | Real hardware (`SU2000.md`) | Driver state |
|---|---|---|
| Host CPU | 486DX-33, ALI M1429 | `I486` @ 33 MHz, no chipset (`:149`) |
| RAM | 8 MB | 3.625 MB mapped (`:104`, `:108`) |
| Storage | 500 MB HDD, 2x IDE CD, floppy | none; `ide_interrupt` is `#if 0` (`:132-137`) |
| Control video | Hualon SVGA, 256 KB, BIOS 2.08 | stock `VGA`, 1 MB VRAM, BIOS ROM present (`:174-176`, `:194`) |
| HMD video path | SVGA -> FORMAT C -> VID 1000 -> visette | absent; `// TODO: Hualon ISA custom` (`:169`) |
| PIX 1000 | dual MC88110 @ 40 MHz, 16 MB VRAM, per-game firmware | absent; disabled block only (`:158-162`) |
| VID 1000 | FIFOs + SRAM, 27 MHz | absent (PLDs only) |
| FORMAT C | Ensoniq ES5506-class + N80C198 + video mixers | absent; U62 ROM loaded but unused (`:202-203`) |
| FORMAT D | cabinet/credit I/O | absent |
| TRACKER | InsideTrak, TMS320C31 @ ~33 MHz, 2 cards | ROMs loaded into `tracker` region, no CPU (`:198-200`) |
| NETWORK | DP8390-class 10Base-2 | absent |
| Sound | Ensoniq Soundscape | `MACHINE_NO_SOUND` (`:302`) |
| Input | joystick/handbar/buttons/card reader | AT keyboard only (`pcat_common`) |
| Broadcast OSD | port `0x320` | not decoded |
| Multiplayer | NET + per-pod state | absent |

## 10. Source quirks and potential bugs

Facts observable in the source, in rough order of importance:

1. **Dead includes/stubs**: `machine/idectrl.h` (`:56`) and
   `ide_interrupt` (`:132-137`) are unused; `machine/pckeybrd.h` (`:57`) is
   needed only indirectly through `pcat_common`. `MC68000_CLOCK` (`:70`) is
   defined and never used.
2. **Undefined symbols in the `#if 0` block**: `tracker_map`, `pix_cpu_a`,
   `pix_cpu_b`, `formatc_map`, and the `MC88110` device type. Enabling the
   block as written is a compile error, so it is a design note, not latent
   code.
3. **All PAL dumps at offset 0** in one region (`:211-290`): last write
   wins; the region is not a usable PLD archive.
4. **16 MB tracker region not zero-filled** in release builds
   (`src/emu/romload.cpp:1430-1434`), since it exceeds the 4 MB auto-clear
   threshold.
5. **RAM size mismatch**: header says 8 MB (`:30`), map provides 3.625 MB
   (`:104`, `:108`).
6. **VRAM size mismatch**: 1 MB allocated (`:176`) vs 256 KB documented for
   the Hualon SVGA.
7. **Tracker clock mismatch**: `XTAL(33'833'000)` (`:71`) vs the documented
   `33.3333 MHz` oscillator.
8. **`MACHINE_NO_SOUND`** is used, which means "sound hardware exists but is
   not emulated" (`src/emu/gamedrv.h:81`) - appropriate here, unlike
   `MACHINE_NO_SOUND_HW`.
9. **Year 1993** on the GAME line (`:302`) predates the documented game
   releases (Dactyl Nightmare 2, Zone Hunter, etc.), consistent with the
   original 2012 commit reusing the platform's introduction year rather than
   a specific game's year.
10. **No save-state support**: no `MACHINE_SUPPORTS_SAVE`, and the driver has
    no `save_item`/`save_pointer` calls of its own.
11. **`pcat_common` + `DS12885(config.replace(), ...)`**: the MC146818 created
    by `pcat_common` is replaced after the fact, so the RTC is a DS12885 with
    128 bytes of NVRAM and a fixed 1 Hz timebase
    (`src/devices/machine/ds128x.cpp:22-30`). The driver repeats the IRQ and
    century setup that `pcat_common` already did, because `config.replace()`
    discards the old device's bindings.
12. **VGA BIOS mapping** relies on the implicit `rom()` region rule
    (`src/emu/addrmap.cpp:1152-1156`); if the region were renamed, the
    `0xc0000` window would silently point elsewhere.

## 11. Running it

```
mame su2000
```

- ROM set name: `su2000`. Required dumped files are the 44 non-`NO_DUMP`
  entries; the 12 `NO_DUMP` files are allowed to be missing
  (`src/emu/romload.cpp:557-568` treats them as "no good dump known",
  a warning rather than a fatal error).
- Expect a `NOT WORKING` warning and no boot device. With only the two BIOS
  images, the 486 starts at `0xfffffff0` into AMIBIOS; POST behaviour
  depends entirely on the stock `i386`/`pcshare` devices, and there is no
  disk to continue from.
- No input ports are defined by the driver; the AT keyboard device's default
  ports are what the UI exposes.
- No `sd2000` entry exists, so SD2000 titles cannot be selected even though
  the header documents them.

## 12. Where to look next (in the MAME tree)

| Need | File |
|---|---|
| Driver itself | `src/mame/misc/su2000.cpp` |
| PC chipset skeleton | `src/mame/shared/pcshare.{h,cpp}` |
| 486 core (protected mode, CPUID) | `src/devices/cpu/i386/i386.{h,cpp}` |
| M88000 core + MC88110 disassembler | `src/devices/cpu/m88000/` |
| TMS320C3x (tracker DSP) | `src/devices/cpu/tms320c3x/` |
| MCS-96 (FORMAT C candidate) | `src/devices/cpu/mcs96/` |
| VGA device | `src/devices/video/pc_vga.{h,cpp}` |
| DS12885 RTC | `src/devices/machine/ds128x.{h,cpp}` |
| DP8390 NIC (network candidate) | `src/devices/machine/dp8390.{h,cpp}` |
| ES5506 (FORMAT C audio candidate) | `src/devices/sound/es5506.{h,cpp}` |
| ROM entry/loader semantics | `src/emu/romentry.h`, `src/emu/romload.cpp` |
| Address-map semantics | `src/emu/addrmap.cpp` |

## 13. Verification notes

Commands used to build this document (all read-only):

- `Get-ChildItem -Recurse -Filter "*su2000*"` and `*su2k*` in the MAME tree -
  only `src/mame/misc/su2000.cpp` matched.
- `git log --follow --date=short -- src/mame/misc/su2000.cpp` - history in
  section 2.
- `git show 2bec585837c:src/mame/drivers/su2000.c` - original driver (had a
  4 MB bank RAM and an early `pc_vga_interface`, both since removed).
- `git show d3a18f29af2` - the CMOS-size / straight-RAM change.
- Line references in this file are from the current working tree at
  `7e7a99fd7b5`.
- Counts (`56` loads, `12` `NO_DUMP`, `44` dumped, `39` dumped PALs,
  duplicate CRC groups) were produced by parsing the `ROM_LOAD` lines in the
  driver, not by running MAME.
