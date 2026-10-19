# VFX1.COM — Full Disassembly Infodump

Source binary: `D:\Emu\DOS\VFX1\vfx1.com` (**52,241 bytes = `0xCC11`**,
modified 1997-01-20 22:09:00). Toolchain: Rizin
(`C:\Apps\rizin-win-installer-vs2019_static-64\bin\rizin.exe` +
`rz-bin.exe`) with `asm.arch=x86`, `asm.bits=16`, `io.va=false`, plus a
Python/Capstone-16 recursive-descent pass. Scope: **the resident driver/TSR
itself** — layout, strings, INT/port census, routine map, boot flow, error
model. Card-level detection background lives in `VFX1.md`; the installer/test
UI lives in `setvfx1.md`. Nothing below comes from any emulator source.

All offsets are **file offsets (hex)** unless marked VA. Runtime VA for a
`.COM` (`ORG 0x100`, `CS=DS=ES=SS`) = file offset + `0x100`. With
`io.va=false`, the Rizin address equals the file offset.

## 1. The 30-second version

1. `VFX1.COM` is a **single-segment DOS `.COM`** (no `MZ`, first bytes
   `E9 05 0C` = `jmp 0xC08`). True entry is file `0x0` → code starts at file
   `0xC08` (VA `0xD08`). Bytes `0x3–0xC07` are header/data/BSS, not code.
2. It parses `VIPPORT` from the environment plus a rich option set
   (`-i -p# -l -m# -y -r -f -s/+s -o/+o -c/+c -t/+t -z -v -q`), probes VGA,
   runs the VIP echo/HWID/CFGSTATUS/IRQ checks, loads DSP firmware blobs
   (`VFX1.F1/F2/F11/F3`), then installs itself resident with `INT 21h
   AH=31h` (keep `DX=[0xCFE]` paragraphs). There is **no `INT 27h`** path.
3. The resident half owns three hooks: **`INT 10h → 0x0E09`** (video/snoop),
   **`INT 33h → 0x1396`** (the VFX-1 driver API, not the mouse), and the
   **VIP hardware IRQ → 0x279A** (ACCESS.bus ISR body). Already-resident
   invocations (`-r/-f/-s/+s/-o/+o/-c/+c/-t/+t`) go through the `0xC47`
   dispatcher, not re-init.
4. All VIP I/O is **`in/out dx`** (`DX`-indirect); there is no immediate-port
   I/O in true code. Base lives in `[0x173]`/`[0xA4C]`/`DI`/`SI` (default
   `0x260`, IRQ default `07`); offsets used: `+0x0` echo/latch, `+0x1`
   data-phase, `+0x2/+0x3` DSP index/data, `+0x8` ACCESS.bus status,
   `+0x9` ACCESS.bus data, `+0xA` IRQCFG, `+0xC/D/E` HWID, `+0xF` CFGSTATUS.
5. "Error 27" is **not a driver error number in this binary**: the error
   accumulator is `[0xCA2]` with observed values 0/5/6/7/0x0B plus
   pass-through `AX`. No `mov ax,27` exists; all `0x1B` bytes are immediates
   inside code/tables. The `ERROR! #$` string prints whatever `[0xCA2]`
   holds.

## 2. Memory / segment / ORG layout

- COM load: `CS=DS=ES=SS` = PSP+`0x10`, `IP=0x100`, `SP` = top of segment.
  No relocations. Entry stub at file `0x0`: `jmp 0xC08`.
- Startup `0xC08–0xC46`: saves entry `DX` to `cs:[0x10D]`, sets `DS=CS`,
  `AH=30h` (DOS version), saves `ES` to `[0x10B]`, zeroes BSS
  `0x3DA–0xD08` (`DI=0x3DA`, `CX=0xD08`, `rep stosb`), clears `[0x109]`,
  `call 0x2C04` (transient main), then `AH=4Ch` terminate.
- PSP handling in `0x2C04`: `[0x10B]` = PSP seg, `[0xC92]=0x80`
  (command-tail length pointer), `[0xC94]` = PSP seg, `[0xC8C]=0x2C`
  (environment segment pointer at PSP+`0x2C`). Parses the command tail via
  `les bx,[0xC92]`, terminates with `0x20`, and parses the program path via
  `\`, `.`, `:` separators.
- Transient (init only, discarded): `0x2C04–0x34CE` — option parser,
  orchestrator, TSR installer. Exit at `0x34CE`: restores `SS:SP` from
  `[0xC86]/[0xC88]`, moves `[0xCA2]` to `AX`, `leave; ret` to the `0xC37`
  caller, which prints `Error from init: $` on nonzero.
- Resident (kept): ISR/dispatcher `0xC47–0x11B1`, VGA/snoop
  `0xDA1–0x1112`, packet/ACCESS.bus `0x1C21–0x28EF`, INT33 API
  `0x2B69–0x300B`, HW tests `0x3AB9–0x40BB`, DSP/file `0x3900–0x39FF`,
  echo `0x4B70–0x4C31`, utils `0x4C5C–0x4D04`, env parse `0x4D07–0x4DA4`,
  plus tables/strings. Keep size `DX=[0xCFE]` at `0x34C2–0x34C9`
  (`mov dx,[0xCFE]; mov ax,0x3100; int 0x21`).
- Stack: transient switches `SS=DS`, `SP=0x641E` (`0x2C10–0x2C14`),
  saving old `SS:SP` at `[0xC86]/[0xC88]`.
- Mixed region `0x34DB–0x4805` is **data + code islands** (binary tables,
  encoded resident data, DSP blobs). Linear sweep mis-decodes it as
  `salc/xor` soup; real code islands include the video helper `0x3884`,
  table init `0x3898`, cached HWID `0x4503`, and the idle check `0x4806`.
- Rizin note: `aa`/`afl` finds only `fcn.00000000` (stub) and
  `fcn.00002C04` (transient main); `aaa` is no better on a raw COM because
  ISR/hook targets are reached indirectly. `iz` returns only the header
  strings (`0x96–0x2D0`, `0x4DA6–0x531F`, `0xCB46–0xCBB8`); `ie` has no entry
  (flat binary). Routine starts below come from the Capstone pass, where
  `enter`-prologue starts are certain and mid-labels are noted as such.

## 3. Meaningful string table (file offsets)

A raw printable-run scan yields 101 runs; all but the following are 4-char
code noise (`PSQR`, `VWU.`, `ZY[X`, `3L3h3`, …). `$` = `INT 21h AH=09h`
terminator. Case-insensitive scan is **negative** for `ATTENTION`, `ACCESS`,
`bus`, `packet`, `firmware`, `mouse`, `INT33`, `send/recv/tx/rx` — the
ACCESS.bus opcodes and INT33 API names are numeric, not strings. `DSP`
occurs once (inside the retry format); `VIP` 8x, `Puck` 4x, `error` 4x,
`snoop` 1x, `palette` 2x, `stereo` 2x, `sync` 2x, `power` 4x, `center` 2x.

| File off | String |
|---|---|
| `0x096` | `VFX1` |
| `0x09B` | `PUCK102` |
| `0x0CC` | `vfx1.ini` |
| `0x0D6` | `SENS` |
| `0x0DB` | `Forte Technologies, Inc.` |
| `0x0F5` | `VFX1 CyberPuck` |
| `0x105` | `000B` |
| `0x10C` | `Base port:  $` |
| `0x11A` | `Interrupt:  $` |
| `0x128` | `Attempt #%d Loading DSP\n` (`%d` = retry counter) |
| `0x141/0x150/0x15F` | `Loading: $` (x3, DSP stages) |
| `0x16E` | `   Internal RAM Conf.: B0 - prog, B1- data, B3 - data\r\n$` |
| `0x1A7` | `   Internal RAM Conf.: B0 - prog, B1- prog, B3 - data\r\n$` |
| `0x1E0` | `   Internal RAM Conf.: B0 - prog, B1- prog, B3 - prog\r\n$` |
| `0x219` | `VIP Hardware Version \r\n$` |
| `0x232` | `Rev Hi: $` |
| `0x23C` | `Rev Lo: $` |
| `0x246` | ` VIP Card base port not detected.  Check VIPPORT setting.\r\n$` |
| `0x283` | `ERROR!  #$` |
| `0x292` | ` Ram error count = $` |
| `0x2AC` | `Program code size (Bytes) :$` |
| `0x2C9` | `.text` (COFF-ish tag in DSP blob header) |
| `0x2D0` | `VIPPORT=` (env-var match token) |
| `0x4DA6` | `Puck is on address: $` |
| `0x4DBC` | `Puck is OFFLINE\r\n$` |
| `0x4DCF` | `VFX1 is on address: $` |
| `0x4DE5` | `VFX1 is OFFLINE\r\n$` |
| `0x4DF8` | `Centering VFX1 Headset\n\r` |
| `0x4E10` | `Press a key when facing forward.\n\r$` |
| `0x4E34` | `Error from init: $` |
| `0x4E47` | `Snooping is: $` |
| `0x4E56–0x51FF` | Help text (~938 B): `-i` init only; `-p#` force base (ignore `VIPPORT`); `-l` no load-high; `-m#` power saver (0=off, def 0); `-y` no polarity; `-r` reset center; `-f` free driver; `-s/+s` palette (def `-s`); `-o/+o` power (def `+o`); `-c/+c` video sync (def `+c`); `-t/+t` line-sequential stereo (def `-t`); `-z` debug; `-v` version; `-q` quiet |
| `0x5202` | `VFX1 Controller Copyright 1995,96 FORTE Technologies Inc.\n\r$` |
| `0x5240` | ` Version 2.13 \n\r$` |
| `0x5253` | ` VIP Host card not configured. (Unloading VFX1)\n\r$` |
| `0x5287` | ` Invalid VFX1 option\n\r$` |
| `0x52A0` | ` VFX1 already resident\n\r$` |
| `0x52BB` | ` VFX1 not loaded\n\r$` |
| `0x52D0` | ` Driver not released but disabled. Use +o option to enable\n\r$` |
| `0x530F` | ` VFX1 released\n\r$` |
| `0xCB46` | `VFX1.F2` (DSP firmware filename) |
| `0xCB4E` | `VFX1.F1` |
| `0xCB56` | `VFX1.F11` |
| `0xCB5F` | `VFX1.F3` |
| `0xCBB8` | `NoError` (DSP blob trailer) |
| `0xC67` area | `C:\vfx1` default-path fragment (near `0xCB67`) |

## 4. INT census (true code only)

A raw `CD xx` scan finds 528 candidates, but most are false positives
(`int3`, `int 0x8B/0x9D/0xEB/0xEE/0xFF`, immediates inside
`push/call/mov`). True code uses only the following:

| File off | INT | Context — purpose |
|---|---|---|
| `0xC18` | `21h` | `AH=30h` get DOS version (entry) |
| `0xC40` | `21h` | `AH=4Ch` terminate (`AL=0`) |
| `0x2964` | `21h` | `AX=25xxh` set vector (`AL`=[bp+4], `DS`=[bp+8]) — util |
| `0x2973` | `21h` | `AX=35xxh` get vector — util |
| `0x2A80/0x2A96` | `21h` | `AH=09h` print `$`-string — util |
| `0x2A9D` | `21h` | `AH=08h` console input no-echo (used by `-r` center prompt) |
| `0x2B98` | `33h` | `AX=DI` — driver call (snoop/palette path) |
| `0x2BCE` | `33h` | `AX=[bp+4]\|8` — driver call |
| `0x2F6B/0x2FD4/0x2FE2/0x2FF8` | `33h` | `BX=1/2/9/0xA/0` driver presence/probe; `0x2FE2` uses `AX=0x60FF` + `DX=[0x173]` |
| `0x33D0` | `21h` | `AX=4900h` free memory |
| `0x343B` | `21h` | `AX=5000h` set PSP (load-high path) |
| `0x3444` | `21h` | `AX=4900h` free memory |
| `0x3469` | `21h` | `AX=2500h AL=10h DX=0x0E09` set **INT 10h** to resident video hook |
| `0x3479` | `21h` | `AX=2500h AL=33h DX=0x1396` set **INT 33h** to resident driver API |
| `0x348A` | `21h` | `AX=2500h AL=[0x7FE] DX=0x279A` set **HW IRQ** to VIP ISR |
| `0x34C0` | `33h` | `AX=0x60FF BX=1 DX=[0x173]` pre-TSR driver ping |
| `0x34C9` | `21h` | `AX=3100h DX=[0xCFE]` **keep resident (TSR)** |
| `0x3580/0x388E/0x399B/0x39A3/0x39AB/0x39BF/0x39C7` | `21h` | `AX=58xxh/4800h` UMB strategy/alloc (`-l` load-high logic) |
| `0x35EA` | `21h` | `AX=4900h` free (after `ES=SI` high-scan) |
| `0x3887` | `10h` | `AX=0F00h` get video mode |
| `0x3893` | `10h` | `AL=[0x107]` set video mode (save/restore around `AH=00h`) |
| `0x3937` | `21h` | `AH=3Fh` file read (DSP blob) |
| `0x394F` | `21h` | `AH=3E00h` file close |
| `0x3971` | `21h` | `AH=3D02h` file open (DSP `VFX1.F*`) |

No `INT 27h`, `INT 13h/15h/2Fh` in true code. `INT 33h` here is the VFX-1
driver API hooked at `0x1396`, not the mouse.

## 5. Port census (all `in/out dx` in true code)

No immediate-port I/O in true code. Every VIP/VGA access is
`mov dx,base; [add dx,off]; in/out dx,al/ax`.

| Ports (DX) | File-off examples | Meaning |
|---|---|---|
| `0x3D4/0x3D5` VGA CRTC idx/dat | `0xDA6/DAA`, `0xDB6/DBA`, `0xDC9/DD1`, `0xDF8`, `0xE49–0x1112` (dozens) | Polarity/snoop mode set + status poll; `0xDA1` VGA detect (`out 3D4,06h/16h; in 3D5; xor`); `0xDC1`: `dx=[0x173]+2 → out 02h`, `+3 → in` |
| `0x3C7/0x3C8/0x3C9` VGA DAC rd-idx/wr-idx/data | `0x1B46/1B5B/1B7A/1B83`, `0x3E0F–0x3E62` | Palette save (`rep insb` from `3C9`, `CX=0x300` at `0x1B99–1B9C`), restore (`+0x55` bias loop `0x3E38–0x3E62`); `-s/+s` option |
| VIP `base+0x0` echo (base=`[0x173]`, `[0xA4C]`, `DI`/`SI` param) | `0x3322/332E/333C` (`out 09h/07h/06h`), `0x1C40`, `0x3E29–0x3E6D`, `0x4BB0–0x4BD0` | Latch/command strobes `09h/07h/06h/08h/0Ah`; echo test writes `[0xCC42]/[0xCC3C]`, loops `0x7D0` reads |
| VIP `base+0x1` | `0x1BA5`, `0x3E2A–0x3E72`, `0x4003–0x4075` | Data-phase byte |
| VIP `base+0x2` | `0xDC5` (`out 02h`), `0x4007–0x406C` (`out 00h/01h/02h`, then read `+3`) | Index for DSP-RAM save/restore (`[0xCD0B/D/F]`) |
| VIP `base+0x3` | `0xDCE/DD1/DF3`, `0x400C–0x4072` (`in al,dx`) | Status/data read |
| VIP `+0x8` status | `0x279D/27B5/27C9`, `0x2819/2855`, `0x3D47` (saved `[bp-1]`) | ACCESS.bus status poll (`test 04h/20h/02h/10h`) |
| VIP `+0x9` data (`[bp-2]=base+9`) | `0x2846` (`out`), `0x27B2–0x27BB` (`in` → `[bx+si]` loop) | TX byte emit / RX byte store |
| VIP `+0xA` IRQCFG | `0x1C45–0x1C4B`, `0x3322` ctx | IRQ config write |
| VIP `+0xC/D/E` HWID | `0x3AD3/3ADC/3AE5` (`in` → `[bp-1/2/3]`), checked `==0x0D,!0xFF,!0xFF` at `0x3AED–0x3AFD`, math via `0x291C` | HWID/version test; cached at `[0xCC3C/3E/40/22]` (`0x4508–0x4522`) |
| VIP `+0xF` CFGSTATUS | `0x3B31–0x3B34` (`in` → `[bp-1]`, returned in `AX`) | Config-status test |
| Cached mirrors `dx=[0xCC30/34/3C/3E/42]`, `[0x175]`, `[0xA4C]` | `0x3323/332F/333D` (`dx=[0x175]`), `0x48DF–0x48E3` (`dx=[0xCC34]`), `0x4BE5–0x4C1B` (`dx=[0xCC30]`) | Echo/data/status mirrors used by DSP loader + idle check |
| PIC `0x20/21/A0/A1`, PIT `0x40–0x43` | none as immediates in true code | Mask/EOI via VIP `+0xA`/DOS vectors, not direct PIC/PIT immediate I/O |

## 6. Routine map (file offsets, one line each)

`VA = off+0x100`. Earlier RE labels such as `0x27EF sender`, `0x2882 RX`,
`0x2232 dispatcher`, `0x279A ISR`, `0x2845 IDREQ`, `0x23AA demuxer`,
`0x28B5/28C9 pump`, `0x4806 idle` are **mid-function offsets**, not starts —
true starts are given here (`0x2782` RX/ISR, `0x27EF` TX, `~0x222C`
dispatcher, `0x28C9` is a divide helper).

| File off | Purpose |
|---|---|
| `0x0000` | `jmp 0xC08` — COM entry stub |
| `0x0C08–0x0C46` | Startup: `DS=CS`, `AH=30h`, zero BSS, `call 0x2C04`, `AH=4Ch` exit |
| `0x0C47–0x0CE8` | Resident INT dispatcher pre-filter: compares `cs:[0x107]` against `03/04/05/09/0D/0E/12/10/0101/0100/010D/010E/010F/0153/0154/0155/13/3A`, routes to `0xC8B`/`0xCE9`/`0xC8E`/`0xD07` |
| `0x0CE9–0x0D9E` | INT handler body: save regs, `DS=[0x103]`, `call 0x1126`, restore, `iret` (`0xDA0`) |
| `0x0DA1–0x0DF9` | VGA CRTC detect + VIP `+2/+3` init (`out 3D4/3D5`, `xor cl,dl`, `or 08h`) |
| `0x0DFA–0x1112` | VGA extended init (dozens of `3D4/3D5` pairs) |
| `0x1126` | INT body worker (called from `0xCFB`) |
| `0x1189–0x119D` | Far-call thunk via `[0x3E6]` |
| `0x119E–0x11B1` | Far-call thunk via `[0x4CA]` |
| `0x11B2–0x11C5` | VIP word poke (`dx=[0x173]`, `dx=[0x175]`) |
| `0x1B40–0x1BEC` | Palette/DAC + VIP probe: `rep insb` `3C9→[0x4FC]` (`0x300` bytes), VIP `09h` strobe |
| `0x1C21–0x1C4F` | VIP `+0x0/+0xA` writer (`popf` restore) |
| `0x1C50–0x1C89` | Sensor-state commit (`[0x1B4–0x1BA]→[0x1B6/0x1B4]`, calls `0x11C6`) |
| `0x1CAE–0x1D2C` | Retry-call helper (`call 0x1CC1` loop) |
| `0x2232(mid)–0x22A9` | ACCESS.bus **dispatcher**: `test [si+4],04h/02h` → flags `[0x4F2]/[0x83E]`, length cap, `0x42`-stride table copy to `[0xAEC]/[0x838]` |
| `0x22AA–0x23A9` | **Demuxer** prologue (`enter 4,0`, `push 0x84`, `ax=[0x1C0]`) |
| `0x23AA–0x249A` | Demuxer body: `mov [0x201],ax`, `call 0x1D2D`, queue management |
| `0x249B–0x268D` | **Orchestrator**: `call 0x1E49/0x2745`, clears `[0x1A5/0x1AE–0x1B2/0x836/0xA56/0xAE2]`, `SI=2` board scan (`[SI+0x838]`, `imul 0x42`, `test [BX+0AEE],80h`) |
| `0x2745` | TX-queue helper (called from orchestrator) |
| `0x2782–0x27EE` | **RX primitive / VIP ISR body** (`enter 2,0`; `dx=DI` base; `in +8`, `test 04h/20h`, `in +9` → `[BX+SI]`; `0x279A` = status-poll inside) |
| `0x27EF–0x286D` | **TX sender primitive** (`enter 8,0`; `DI` base, `SI=base+8`, `[bp-2]=base+9`; `in +8`, mask `BEh/08h`, `test 02h/10h`; `0x2846` = data-emit `out +9`) |
| `0x286E–0x288B` | 4 dispatch stubs (`CX=0/1/2/3`, `push cs/cx`) → common `0x288B` |
| `0x288B–0x290F` | CX-demux common (`mov di,cx`, args `[bp+0xA/E/D/10]`) |
| `0x28C9–0x28ED` | 32-bit divide helper (`shl/rcl/loop 0x20`; the old `pump 0x28B5/28C9` label points here — **not a pump**) |
| `0x2B69–0x2C03` | INT33 API front (`BX=1/2`, `AX=0x60FF`, `int 33h`, string `push/call 0x2A86`) |
| `0x2C04–0x34CE` | **Transient main**: PSP/cmdline parse, `call 0x2AA0/0x4D07`, option state machine, error codes to `[0xCA2]` |
| `0x3320–0x3352` | VIP init sequence (`out 09h/07h/06h`, `in [0x175]`, `sti`, `call 0x3ED6`) |
| `0x33D0–0x34DA` | TSR installer tail (mem mgmt, hook INTs, `AH=31h`) |
| `0x3553` | Relocate/resize helper (`[0xCFE],[0xC7A],[0x10B]`) |
| `0x3576–0x35EE` | UMB load-high (`AH=58h/48h/49h`) |
| `0x3884–0x3897` | Video get/set (`AH=0Fh/00h`, `[0x107]`) |
| `0x3898–0x392F` | Table init (`[0x118]=0x20B`, `[0x11A]=0x13`, …) + file-read wrapper (`AH=3Fh`) |
| `0x3946–0x3958` | File close (`AH=3Eh`) |
| `0x3959–0x3989` | File open (`AH=3Dh`, `VFX1.F*`) |
| `0x3AB9–0x3B27` | **HWID test** (`+0xC/D/E`, `0x0D/FF/FF` check) |
| `0x3B28–0x3B3C` | **CFGSTATUS test** (`+0xF`) |
| `0x3C17` | Echo probe (called by IRQ installer) |
| `0x3C64` | Post-IRQ helper |
| `0x3D38–0x3D58` | IRQCFG helper (`+0x8`, `OR 41h AND 77h`) |
| `0x3D59–0x3DCD` | **IRQ test/hook**: `call 0x3C17/0x3AB9/0x3D38`, get/set vectors (`0x296A/0x2954` → `0x279A`, IRQ `[0x7FE]`), `call 0x1C21/0x3C64/0x1CAE`, RX `0x2782` loop |
| `0x3DCE–0x3ED6` | DSP-RAM / palette engine (`3C7/3C8/3C9` + VIP) |
| `0x4000–0x403C` | DSP-RAM save (`+2` idx `00/01/02`, `+3` → `[0xCD0B/D/F]`) |
| `0x403D–0x4076` | DSP-RAM restore (reverse) |
| `0x4503–0x4579` | Cached HWID check (`[0xCC3C/3E/40/22]`, strings `0x319/332/33C/346`) |
| `0x4806–0x4931` | **Idle check**: `jmp 0x4931` exit + `[SI]==0` tests via `0x4935` comparator |
| `0x4B70–0x4C31` | **Echo-latch test** (`[0xCC42/3C]` out, `0x7D0`x `in [0xCC3E]`, data via `[0xCC30]`, `call 0x4C32` delay) |
| `0x4C32–0x4C5B` | Delay/poll helper |
| `0x4C5C–0x4D04` | `strncmp`/`memcmp` + atoi helpers |
| `0x4D07–0x4DA4` | **VIPPORT env parse**: scan env for `VIPPORT=` (`0x4C5C`), parse (`0x2998`), default base `0x260` / IRQ `07` (`[0xA48]`) |

## 7. Boot / TSR flow

1. Load `ORG 0x100`, `jmp 0xC08`. Save context, zero BSS, `call 0x2C04`.
2. `0x2C04`: snapshot `SS:SP`, switch to `SP=0x641E`, parse PSP
   cmdline/path, `call 0x4D07` → VIP base (`[0x173]`/`[0xA4C]`, default
   `0x260`) + IRQ (`[0xA48]`, default `07`).
3. Option loop: `-i` init-only; `-p#` override base; `-l` no UMB; `-m#`
   power; `-y` no polarity; runtime `-r/-f/-s/+s/-o/+o/-c/+c/-t/+t/-z/-v/-q`.
   Bad option → `Invalid VFX1 option` + `ERROR! #` path.
4. Probes: VGA (`0xDA1`), echo-latch (`0x4BB0`), HWID (`0x3AB9`),
   CFGSTATUS (`0x3B28`), IRQ hook (`0x3D59` → `INT 21h 25h` to `0x279A`),
   DSP load (`Attempt #%d`, `Loading:`, `Internal RAM Conf.`,
   `VFX1.F1/F2/F11/F3` via `3Dh/3Fh/3Eh`), `VIP Hardware Version / Rev
   Hi/Lo` prints.
5. Resident install: UMB (`58h/48h`), `50h/49h`, hook `INT 10h→0x0E09`
   (snoop), `INT 33h→0x1396` (API), HW IRQ→`0x279A`, `INT 33h` ping,
   `AH=31h DX=[0xCFE]` keep. Messages: `already resident / not loaded /
   released / disabled use +o / not configured unloading`.
6. Already-resident invocation (`-r/-f/-s/+s/-o/+o/-c/+c/-t/+t`) goes through
   the INT33 dispatcher (`0xC47`), not re-init. `-r` prints `Centering...
   Press a key...` (`AH=08h` at `0x2A9D`).

## 8. Error model

Error accumulator = **`[0xCA2]`** (returned in `AX` at `0x34D6` →
`Error from init: $` / `ERROR! #$`). Observed values:

| `[0xCA2]` | Trigger (file off) |
|---|---|
| `0` | Success (`0x2F12`, `0x330B`) |
| `5` | Runtime/option failure (`0x3033`) |
| `6` | Init failure (`0x328C`) |
| `7` | VIP detect/init failure (`0x3377`, after `call 0x3ED6` nonzero → `0x3369–0x337D`) |
| `0x0B` (11) | Config/option error (`0x2E7D`, prints `0x5302`+`0x5340`) |
| `AX` | Propagated from `call 0x2A86/0x2745/0x1D2D` (`0x2FE4`, `0x309D`) |

`[0xCA0]` (`0/1` at `0x2DEC/30E3/327D`) is a sub-status. Related strings:
`ERROR!  #$` (`0x283`) + number; ` Ram error count = $` (`0x292`, DSP RAM
verify); ` VIP Card base port not detected.  Check VIPPORT setting.`
(`0x246`); `Error from init: $` (`0x4E34`); ` VIP Host card not
configured. (Unloading VFX1)` (`0x5253`); ` Invalid VFX1 option` (`0x5287`);
` VFX1 already resident` (`0x52A0`); ` VFX1 not loaded` (`0x52BB`);
` Driver not released but disabled. Use +o option to enable` (`0x52D0`);
` VFX1 released` (`0x530F`); `Puck/VFX1 is OFFLINE` (`0x4DBC/0x4DE5`);
`NoError` (`0xCBB8`, DSP blob trailer).

## 9. Emulator notes (what this binary needs)

- VIP base/IRQ come from the `VIPPORT` environment string (`SET
  VIPPORT=2x0 Iy`); `-p#` overrides. Defaults `0x260`/`I7` when absent.
- Detection order the responder must satisfy: echo-latch on `+0x0`, HWID
  `0x0D` + `HWV != 0xFF` on `+0xC/D/E`, CFGSTATUS on `+0xF`, IRQCFG on
  `+0xA`. All are `DX`-indirect; no `OUT imm`/`IN imm` fast path exists.
- The driver never programs PIC/PIT directly — IRQ routing is via DOS
  vectors + VIP `+0xA`, so unmask/raise behaviour must be supplied by the
  responder, not expected from guest `OUT 0x21/0xA1`.
- Repro recipe: `rizin -q -c "e asm.arch=x86; e asm.bits=16; e
  io.va=false; s 0; aa; afl; iz" vfx1.com`, then Capstone-16 from the
  `enter`-prologue starts above; grep ` EC ` / ` EE ` (`in al,dx` /
  `out dx,al`) and `CD 21/33/10` for the censuses. Region `0x34DB–0x4805`
  must be treated as mixed data/code, not linearly swept.

## 10. Claim map

| # | Claim | Evidence |
|---|---|---|
| 1 | `.COM`, `ORG 0x100`, entry `jmp 0xC08` | First bytes `E9 05 0C`; startup at `0xC08` |
| 2 | Size `0xCC11`, date 1997-01-20 | `Get-Item` length + timestamp |
| 3 | TSR via `AH=31h`, no `INT 27h` | `0x34C9` keep; raw+visited scan negative for `27h` |
| 4 | Hooks `INT 10h→0x0E09`, `INT 33h→0x1396`, HW IRQ→`0x279A` | `0x3469/0x3479/0x348A` |
| 5 | All VIP I/O `DX`-indirect | Capstone census; zero immediate-port I/O in true code |
| 6 | Error accumulator `[0xCA2]`; no error-27 immediate | `0x34D6` return; scan negative for `mov ax,27` |
| 7 | DSP blobs `VFX1.F1/F2/F11/F3` via `3Dh/3Fh/3Eh` | Strings `0xCB46–0xCB5F`; file calls `0x3937/0x394F/0x3971` |
| 8 | 2.14 = 2.13 + one `and` insn + version bump (see §11) | 264-diff census: 263 mechanical ±6 + `0x33→0x34` |

## 11. Version delta: 2.13 vs 2.14 (`D:\Emu\DOS\VFX1` vs `D:\Emu\DOS\C\VRDRV\VFX1`)

Copies inventoried (SHA256): 2.13 `52241 B, 1997-01-20,
E1DA00DB…821459` — byte-identical in `D:\Emu\DOS\VFX1`,
`D:\Emu\DOS\Y-utils\VFX1`, `D:\Emu\DOS\C\VRDRV\vfx1sdk\VFX1`; 2.14
`52247 B, 1997-10-14, 6F79573B…3893DE5D` — byte-identical in
`D:\Emu\DOS\C\VRDRV\VFX1`, `D:\Emu\DOS\C\VFX215`,
`D:\Emu\DOS\C\VFX216` (both newer trees carry the 2.14 driver).
Version strings: ` Version 2.13 \n\r$` at 2.13 file `0x5240` vs
` Version 2.14 \n\r$` at 2.14 file `0x5246` (string block shifted +6).

The **only** code change is one 6-byte instruction inserted at 2.14 file
`0xD8F` (2.13 has `popf` there), in the chained-handler exit path:

```
2.13 @0xD8B:  mov  word cs:[0x107], ax   ; 2E A3 07 01
2.13 @0xD8F:  popf
2.14 @0xD8B:  mov  word cs:[0x107], ax   ; 2E A3 07 01
2.14 @0xD8F:  and  word cs:[0x107], 0x7F ; 2E 83 26 07 01 7F (INSERTED)
2.14 @0xD95:  popf
```

Semantics: clears bit 7 of the INT-dispatcher routing cell `[0x107]`
right after the previous-handler chain (`call far cs:[0x3F2]`) and the
`call 0xC47` dispatch probe. The `0xC47` chain (§6) only matches the
listed 7-bit patterns (`03…3A`, `0100…0155`); an incoming `AX` with bit
7 set previously fell through to the wrong leg (`jmp 0xD07` vs the
`0xCE9` handler body). 2.14 folds it into 7-bit routing space. The exact
mistriggered caller is not identified from this delta alone (no other
bytes name it) — best-effort reading, stated as such.

Completeness proof (positional diff, then shift-compensated recount):
pre-insert region `0–0xD8E` identical except 2 mechanical +6 forward
rels (file `0xC38` call `E8 CA 1F→D0 1F`, file `0xCFC` jump
`28→2E`); post-insert 264 diffs = 263 mechanical ±6 (forward
rel16/rel8 +6, backward `call 0xC47` −6 at a-`0xD97`, absolute
near-pointer tables incl. the `0x1AC9–0x1B31` block, transient/TSR
refs) + exactly 1 non-±6 byte: a-`0x524C` `0x33→0x34` (the version
string). File tail gains 6 zero pad bytes (`0xCC11–0xCC16`). Net delta:
**one instruction + version bump + padding**.

Offset rule: this doc's file offsets are 2.13-based
(`D:\Emu\DOS\VFX1\VFX1.COM`); add **+6** to any offset ≥ `0xD8F` for
the 2.14 copy. Bundle composition (driver × setup label, see
`setvfx1.md` §11 for the setup side): 2.13 bundle = driver 2.13 +
setup 1.20a/`2.13`; 2.15 bundle (`D:\Emu\DOS\C\VFX215`, from
`vfx1_215.exe`) = driver **2.14** + setup 1.20a/`2.15`; 2.16 bundle
(`D:\Emu\DOS\C\VFX216`, same pair as `VRDRV\VFX1`) = driver **2.14** +
setup 1.20a/`2.16`. So no bundle ever shipped a driver newer than
2.14 — the follow-up is closed.
