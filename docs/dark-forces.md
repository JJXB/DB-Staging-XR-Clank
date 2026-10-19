# Dark Forces — VFX1 Driver Infodump

Source binary: `D:\Emu\DOS\C\darkvfx\DARKVFX1.EXE` (**48,744 bytes = `0xBE68`**, modified 1995-03-23), a 32-bit LE/DOS-extender executable (arch x86, bits 32, OS/2 per rizin). Related: `DFVFX1.EXE` (44,326 bytes, same dir, dated 2026-10-15). Scope: the VFX1 driver/TSR that adds VFX1 Headgear + CyberPuck support to LucasArts Dark Forces (CD-ROM release required). Nothing below comes from emulator source — only strings + rizin disassembly of the DOS binary.

## 1. The 30-second version

`DARKVFX1.EXE` is a **Vfx1 Controller for Dark Forces (Version 1.0)** by FORTE Technologies, Inc. It is run before Dark Forces, installs itself as a TSR/ACI-layer driver, prompts the user to center the headset, then spawns `dark.exe` with VFX1 tracking active. It communicates with the VFX1 VIP card through an ACI ("Sense") device layer rather than bare VIP port I/O — the strings reference "ACI application", "ACI Software vector", "No Sense devices installed", and "configuration packets".

## 2. Strings (file offsets, virtual = file + 0x20000)

| File off | String |
|---|---|
| `0x7604` | `/Aapplication.exe: example /Adark.exe\n` |
| `0x762b` | `/VXX: example /V2B Use interrupt vector 2B hex.\n` |
| `0x765c` | `/P:  Turn off the Cyber puck.\n` |
| `0x767b` | `/T:  Turn off the VFX1 tracking.\n` |
| `0x769d` | `/S:  Turn on strafing from VFX1 head tilting.\n` |
| `0x76cc` | `/L:  Use the VFX1 head yaw movement to move the legs.\n` |
| `0x7703` | `/F#: Set the speed multiplier for forward motion(0-5 Default 3).\n` |
| `0x7745` | `/Y#: Set the speed multiplier for left/right motion(0-5 default 3).\n` |
| `0x778a` | `/D:  Turn on debug prints.\n` |
| `0x77a6` | `dark.exe` |
| `0x77b0` | `Bad option\n` |
| `0x77bc` | `Error getting DOS memory!\n` |
| `0x77d7` | `No Sense devices installed` |
| `0x77f3` | `Error#(%d) getting configuration packets\n` |
| `0x781d` | `VFX1 CyberPuck` |
| `0x782d` | `Error#(%d) enabling configuration packets\n` |
| `0x7858` | `\n%s\n` |
| `0x785d` | `VFX1 Head tracker device installed\n` |
| `0x7881` | `CyberPuck Pointing device installed\n` |
| `0x78a9` | `ACI application:       %s\n` |
| `0x78c4` | `ACI Software vector:   %x\n` |
| `0x78df` | `Forward multiplier:    %x\n` |
| `0x78fa` | `Left/Right divider:    %x\n` |
| `0x7915` | `Argument:              %s\n` |
| `0x7930` | `ATTENTION: FACE Forward and Press any key` |
| `0x795b` | `Error from spawning dark forces.\n` |
| `0x7983` | `.cmd` |
| `0x7988` | `.bat` |
| `0x798d` | `.com` |
| `0x7992` | `.exe` |
| `0x7997` | `COMSPEC` |
| `0x79a3` | `COMMAND` |
| `0x79ac` | `Floating-point support not loaded\r\n` |
| `0x79d0` | `_tempa.tmp` |
| `0x7ae4` | `\nVfx1 Controller for Dark Forces (Version 1.0)\n` |
| `0x7b1a` | `Stack Overflow!\r\n` |

## 3. Command-line interface

Matches `DARKVFX1.TXT` exactly:

| Option | Meaning | Default |
|---|---|---|
| `/A<exe>` | Target application EXE (if renamed from `dark.exe`) | `dark.exe` |
| `/Vxx` | Interrupt vector in hex (2A, 2B, 2C, 2D on conflict) | `2B` |
| `/P` | Turn off CyberPuck | on |
| `/T` | Turn off VFX1 head tracking | on |
| `/S` | Strafe on head roll (tilt) | off |
| `/L` | Yaw moves legs instead of looking | off |
| `/F#` | Forward speed multiplier 0-5 | 3 |
| `/Y#` | L/R turn speed multiplier 0-5 | 3 |
| `/D` | Debug prints | off |
| `/?` | Help | — |

`/V` selects the software interrupt vector the driver hooks (default 0x2B). Other vectors to try on conflict: 2A, 2C, 2D.

## 4. Runtime behaviour (disassembly-verified)

Entry at `0x11300` (`jmp 0x1136b`). Startup does INT 21h AH=30h (DOS version), checks for a DOS extender signature (`XD`/`CB`), sets up segment registers and memory, then enters the command-line parser at `0x105ee`–`0x109cb` which scans `/`-prefixed options character by character (matches `/A`, `/V`, `/P`, `/T`, `/S`, `/L`, `/F`, `/Y`, `/D`, `?`, `-`).

Device detection flow (`0x10ced` onward):
- Prints `\nVfx1 Controller for Dark Forces (Version 1.0)\n` (string `0x204e4`)
- Checks flag `[0x2088c]` → if set, prints `VFX1 Head tracker device installed\n` (`0x2025d`)
- Checks flag `[0x208bc]` → if set, prints `CyberPuck Pointing device installed\n` (`0x20281`)
- Prints `ACI application: %s`, `ACI Software vector: %x`, `Forward multiplier: %x`, `Left/Right divider: %x`
- Prompts `ATTENTION: FACE Forward and Press any key` (`0x20330`)
- Spawns `dark.exe` via COMSPEC/COMMAND; on failure prints `Error from spawning dark forces.\n` (`0x2035b`)

VFX1 detection calls (`0x1119d`, `0x111e9`):
- `0x1119d`: `call 0x119aa` with `AX=0x31` — sets sense-device config word at `[0x20902]=0x101`, data at `[0x2090E]`, then reads back `[0x2091A]` (zero → success). Calls `0x112A3` stack-probe helper.
- `0x111e9`: `call 0x119AA` with `AX=0x30` — reads 32 bytes from `[0x20D1C]`/`[0x20D22]`, stores data pointers at `[0x20906]`/`[0x20912]`; on failure prints `No Sense devices installed` (`0x201d7`) or `Error#(%d) getting configuration packets` (`0x201f3`).
- `0x112A3`: stack-overflow probe (`cmp esp,[0x20560]`; on fail prints `Stack Overflow!` `0x2051A`).

INT census (true code): INT 21h (DOS services, AH=30h version, AH=4Ch terminate, AH=4Ah resize, INT 25/26 absolute disk via hook chain), INT 1A (time of day). No direct VIP `IN`/`OUT` immediate-port I/O found in the disassembly — all hardware access goes through the ACI/Sense layer at vectors `0x2500`–`0x2506`.

## 5. VFX1 feature map (per DARKVFX1.TXT)

- Headset: look/aim up-down (pitch), look left-right (yaw; body stays fixed)
- CyberPuck: forward/reverse motion, turn left-right, buttons top→bottom = fire/use/select-weapons; hold button 2 + click button 3 = jump
- `/S`: head-roll strafing; `/L`: yaw drives legs (disables side-looking)

## 6. Claim map

| # | Claim | Evidence |
|---|---|---|
| 1 | `DARKVFX1.EXE` is FORTE VFX1 driver v1.0 for Dark Forces CD | Strings `0x7AE4`, `DARKVFX1.TXT` |
| 2 | Uses ACI/"Sense" device layer, not direct VIP port I/O | Strings `ACI application`, `ACI Software vector`, `No Sense devices`; disassembly shows no immediate-port I/O, calls via `0x25xx` vectors |
| 3 | Hooks INT vector selectable via `/V` (default 0x2B) | Strings `/VXX`, disassembly at `0x1088d`–`0x109cb` vector dispatch |
| 4 | Spawns `dark.exe`, prints error on spawn failure | Strings `dark.exe` `0x77A6`, `Error from spawning dark forces` `0x795B`; spawn code path at `0x10EB0`+ |
| 5 | Centering prompt `ATTENTION: FACE Forward...` before launch | String `0x7930`, referenced at `0x10D34` |
