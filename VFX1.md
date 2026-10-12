# VFX1 VIP Card Detection — SDK Infodump

Source tree surveyed: `D:\Emu\DOS\C\VRDRV\vfx1sdk` (**every** file opened:
`README.TXT`, `VFX1SDK.EXE`/`vfx1_213.exe` installers, all of `VFX1/` incl.
`DOSSDK/QSTART|EXAMPLE1|EXAMPLE2`, `VFX1.COM`, `SETVFX1.EXE`, `VFX1DIAG.EXE`,
`VRMOUSE.COM`, `*.INI`, `*.FAQ`, `*.BAT`, and all of `doc/` + `doc/doc/`).
Scope of this file: **how the software finds out a VIP card is present,
which port/IRQ it is on, and whether it is usable**. Application-level
tracking/video APIs are summarized only where they matter for detection.
Nothing below comes from any emulator source — only the SDK materials and
disassembly of the SDK's own DOS binaries.

## 1. The 30-second version

1. The VIP is a **16-bit ISA card** (ACCESS.bus host + video buffer). Its
   I/O base is set by a **hardware jumper block** (4 pins, 2 hoods) at the
   bottom of the card. Legal bases: **`0x260, 0x280, 0x2A0, 0x2C0`**.
   Legal IRQs: **5, 7, 10, 11, 12, 15**.
2. DOS software is told the card's address through the **`VIPPORT`
   environment string**, e.g. `SET VIPPORT=2A0 I11`, written into
   `AUTOEXEC.BAT` ahead of `VFX1.COM`. `VFX1.COM -p#` can force/override it.
3. Probing a candidate base `B` is pure ISA port I/O on a **16-register
   window `B+0x0 … B+0xF`**:
   - **Echo-latch test** on `B+0x0`: `OUT 0x0A → IN` must read back `0x0A`,
     then `OUT 0x05 → IN` must read back `0x05` (done with interrupts
     disabled). Fail codes 1 / 2. This proves registers, not just bus float.
   - **Hardware-ID test** on `B+0xC / +0xD / +0xE`: `HWID` must equal
     **`0x0D`**; `HWVLOW` and `HWVHI` must each **not** be `0xFF`. The three
     bytes are then folded into a version word (`HWVHI<<8 | HWVLOW`, OR'd
     with `HWID`). A zero result = "no card".
   - **Config-status test** on `B+0xF`: `(IN & 0x06) == 0x06` means usable.
   - **IRQ config** lives at `B+0xA` (write combined value, read back to
     verify), with `B+0xF` written with `0x00` first as part of the setup
     sequence.
4. `SETVFX1.EXE` (interactive `SETUP VIPPORT` screen, F1–F4 = the four bases)
   runs the port test **and** an IRQ conflict test (it knows about
   SoundBlaster / Gravis Ultrasound / mouse IRQ usage via
   `CheckBlasterIRQ`, `CheckUltrasndIRQ`, `CheckMouseIRQ`), reports
   `TEST FAILED / CONFLICT / DETECT`, and rewrites `AUTOEXEC.BAT` on accept.
5. `VFX1.COM` at boot reads `VIPPORT`, runs the HWID/version check, prints
   `VIP Card base port not detected. Check VIPPORT setting.` (and unloads
   with `VIP Host card not configured.`) when it fails. It also prints
   `Base port: / Interrupt: / VIP Hardware Version / Rev Hi: / Rev Lo:`.
6. Above the hardware sits the **INT 33h "SENSE" driver API**. Apps do *not*
   touch ports; they scan function masks `0x60–0x70` with
   `AX = mask<<8 | 0x7F` (`SENSE_DRIVER_STATUS`) to find the driver, then use
   `GET_CONFIGURATION / SET_CONFIGURATION / REPORT` packets. So there are
   two distinct "detections": **card present on ISA** (ports) vs. **driver
   loaded** (INT 33h).
7. The helmet/CyberPuck themselves are **ACCESS.bus (I2C-family) devices**
   behind the VIP, *not* ISA devices: host address `0x50`, HMD powers up on
   default address `0x6E`, sends `ATTENTION`, answers `ID request`, gets an
   address assigned, answers `capabilities`, receives `power-management` and
   `enable-application-reporting`, then streams 12-byte tracking packets.
   The VIP must finish ISA detection before any of this is reachable.

## 2. Hardware facts that constrain detection

From `VFX1manual.doc` (User's Manual), `VFX1_12.FAQ`, `VFX1/README.TXT`:

- Card name: **VFX1 HEADGEAR Interface Protocol (VIP) Card** — ACCESS.bus
  controller + video buffer for the headset (FAQ `[3-5]`).
- Bus: **free 16-bit ISA expansion slot**, edge connector fully seated.
  System requirement: 386+, VGA card with **VESA-standard feature
  connector**, 500 KB disk. (`README.TXT §5`, FAQ `[7]/[7-1]`.)
- **Jumper = address.** "The VIP Card has a jumper at the bottom of the card
  for selecting the Input/Output (I/O) address … a block with 4 pins and two
  removable plastic hoods." (Manual ch. 3.) Only the four bases above exist;
  `README.TXT` writes it as `SET VIPPORT=2x0 Iy` where `x` ∈ {6,8,A,C} and
  `y` ∈ {5,7,10,11,12,15}.
- **VESA feature cable orientation matters electrically**: 26-pin (2×13),
  unkeyed; reversed/misaligned by a pin destroys chips on the VIP
  (`connectors VFX1.doc`). A dead card from a reversed cable reads as "not
  detected" — worth remembering before blaming software.
- Connectors on the card (from `connectors VFX1.doc`):
  - VESA feature connector (to VGA card) — DAC bits PD0–PD7, DAC clock,
    blanking, H/V sync, grounds, select-internal lines.
  - Helmet link HD26 (video L/R + syncs, audio L/R, mic, **pins 08/09 =
    ACCESS.bus B/A**, power/grounds).
  - ACCESS.bus 4-pin Molex SEMCONN (SCL, SDA, GND, +5 V) for CyberPuck —
    same bus the helmet uses.
- Troubleshooting order in the manual (ch. 5): reseat ISA card → check
  feature cable pin 1 → remove card to isolate I/O conflicts → change base
  → `SETUP VIPPORT` → IRQ-conflict check (sound symptom 7 explicitly calls
  out the IRQ test as "good, but not foolproof").

## 3. Software configuration surface (what the user/detection sees)

| Item | Value | Source |
|---|---|---|
| Env string | `SET VIPPORT=<base> I<irq>`, e.g. `SET VIPPORT=260 I7` (installer default) | `VFX1/README.TXT §SETVFX1`, Manual App. A (`@Set VIPPORT=xxx Iy`), `SETVFX1.EXE` strings (`VIPPORT`, `260 I7`, `@SET VIPPORT=`) |
| `AUTOEXEC.BAT` order | `SET VIPPORT=…` → `C:\VFX1\VFX1.COM` → `PATH C:\VFX1;…` | `VFX1/README.TXT` |
| `VFX1.COM` switches | `-i` init hardware only; `-p#` force base port (ignores `VIPPORT`); `-o` re-enable; `-M/-y/-s/-c/-l` power-save/palette/sync/load-high tweaks | `VFX1.COM` help strings |
| `SETVFX1` CLI | args `h/H/?` = help; special arg runs **only** the `SET VIPPORT` subroutine | `SETVFX1.EXE` strings |
| `SETVFX1` UI | `F1 SETUP VIPPORT` menu; `F1 260 / F2 280 / F3 2A0 / F4 2C0` buttons; `BASEPORT / INTERRUPT / TEST RESULTS REPORT / PRESENT CHOICE / DETECT / VIP CARD / JUMPERS / MODIFY AUTOEXEC.BAT REPORT`; errors `ERROR:001–003` (autoexec open/backup/disk-full) | `SETVFX1.EXE` strings |
| Driver TSR | `VFX1.COM` stays resident; `VRMOUSE.COM` (INT 33h keyboard/mouse emulator, configured by `VRMOUSE.INI`) and apps are clients, never probers | `VRMINI.TXT`, `VRMOUSE.INI`, `EXAMPLE*.C` |

## 4. ISA register map used by detection

From the three `VFX1.H` copies (`DOSSDK/QSTART/VFX1.H` is the richest;
`EXAMPLE1/EXAMPLE2 VFX1.H` agree on the bus offsets):

```c
/* VIP I/O Bus Register offsets (added to base B) */
VIP_AIR    0x0   /* address/index register (AKA echo latch in probe) */
VIP_CSR    0x1
VIP_DFIN   0x2   /* data FIFO in  */
VIP_DFOUT  0x4   /* data FIFO out */
ABUS_CONTROL 0x8 /* ACCESS.bus control */
ABUS_DATA    0x9 /* ACCESS.bus data    */
VIP_IRQCFG   0xA /* IRQ config register */
VIP_HWID     0x0C /* must read 0x0D */
VIP_HWVLOW   0x0D /* must != 0xFF (rev low) */
VIP_HWVHI    0x0E /* must != 0xFF (rev high) */
VIP_CFGSTATUS 0x0F /* (IN & 0x06)==0x06 => usable */
// QSTART VFX1.H additionally:
VIPTIMEOUT 1000 /* min 1000 us VIP timeout */
ABUS_CONTROL bits: PC2ABREQ 0x01, PC2ABACK 0x02, AB2PCREQ 0x04,
  AB2PCACK 0x08, PC2ABAV1 0x10, AB2PCEMPTY 0x20, DSPINTR 0x40, PCIEN 0x80
Internal (AIR-selected) regs: SCR 0, SSR 1, ICR 2, ISR 3, PSR 4, VRR 5,
  PCR 6, PPMR 7, PRAR 8, PWAR 9, PCRR 10
```

Window size is 16 ports (`B … B+0xF`); the four legal windows are
`0x260–0x26F, 0x280–0x28F, 0x2A0–0x2AF, 0x2C0–0x2CF`.
(`SETVFX1.EXE` contains the literal table
`60 02 | 80 02 | A0 02 | C0 02` at file offset `0x59AE`.)

## 5. The probe sequence (disassembled from the SDK's own binaries)

### 5a. `SETVFX1.EXE` — `VIP_Test_Port`-family (16-bit, `cli`-guarded)

Entry convention: candidate base passed on stack (`mov si,[bp+6]`, data
segment `0x2A54`). All IN/OUT below use `DX = base + offset` with byte
`AL` transfers (`IN AL,DX = EC`, `OUT DX,AL = EE`).

```
cli
DX = B+0x0 ; OUT AL=0x0A        ; write echo pattern 1
DX = B+0x0 ; IN  AL ; CMP 0x0A  ; else fail-var = 1
DX = B+0x0 ; OUT AL=0x05        ; write echo pattern 2
DX = B+0x0 ; IN  AL ; CMP 0x05  ; else fail-var = 2
DX = B+0xC ; IN -> HWID   (save [bp-1])
DX = B+0xD ; IN -> HWVLOW (save [bp-2])
DX = B+0xE ; IN -> HWVHI  (save [bp-3])
sti
if fail-var != 0: return 0 (DX:AX = 0)
if HWID != 0x0D:            return 0
if HWVHI == 0xFF:           return 0
if HWVLOW == 0xFF:          return 0
version = (HWVHI << 8) | HWVLOW, OR HWID   ; 32-bit lcall shift-or, cl=8
return version (DX:AX != 0 means "card here")
```

Why two echo values: `0x0A` (`1010`) and `0x05` (`0101`) are bitwise
complements — they catch stuck high/low data lines and address-decode
aliases that a single pattern would miss. `0xFF` rejection on the version
bytes catches a floating (pulled-up) ISA bus with no card answering.

### 5b. `VFX1.COM` — boot-time check (same constants, TSR flavor)

Same three reads at `B+0xC/D/E` (`add dx,0x0c/0x0d/0x0e` at `0x3AD3…`),
same accept rule (`HWID==0x0D`, neither version byte `0xFF`), same
shift-`OR` version build, `0` = absent. Differences: no echo-latch write
(the TSR trusts the latch-tested value path and goes straight to ID), and
failure prints the canonical message:

```
VIP Card base port not detected.  Check VIPPORT setting.
VIP Host card not configured. (Unloading VFX1)
```

plus informational `Base port: / Interrupt: / VIP Hardware Version /
Rev Hi: / Rev Lo:` lines.

### 5c. Status / IRQ / FIFO helpers around the probe

Disassembled alongside the probe in both binaries:

- `CFGSTATUS` (`B+0xF`): `IN; AND AX,6; CMP 6 → AX=1 usable else 0`.
- `IRQCFG` (`B+0xA`): read (`DX=B; add dx,0xA; IN`) and write paths; setup
  writes `B+0xF ← 0x00` then `B+0xA ← combined(irq,mode)` with the mode nibble
  built as `(al & 0x7F) | (mode<<4)` under `cli`.
- `CSR` (`B+0x1`) / `DFIN/DFOUT` (`B+0x2/0x4`) sequences: `OUT B,07 →
  OUT B+1,FF → OUT B,06 → IN B+1` loops and FIFO `OUTSB/INSB` bursts —
  firmware download and bus-control traffic, *after* detection succeeds.
- VGA interplay: one helper reads ports `0x3D4/0x3D5` (CRT index/data,
  indices `0x06/0x16`, mask `0x01`) — video-card state consulted during
  setup, not part of card presence.
- `VFX1DIAG.EXE` reuses the same `VIPPORT` string and INT 33h config/report
  calls; its unique strings are VESA mode failures, i.e. it tests the
  *display path*, not the ISA probe.
- `PC2ABACK` (bit 1 of `B+0x8`) is VIP-driven status with send/retry
  semantics in `VFX1.COM`'s packet sender (`@file 0x27EF`, caller `@0x1DC1`):
  the sender writes `(val & ~0x41) | 0x08`, returns 0 (caller resends) when
  BACK is set, and otherwise streams up to N bytes to `B+0x9` (while
  `PC2ABAV1` is set) and returns nonzero (caller done). Bits the guest sets
  in its own `+0x8` control writes are meaningless to real hardware — the VIP
  deasserts BACK once it consumes the ACK — so an emulator must never echo
  bit 1 back, or the sender's retry-while-zero loop spins forever
  (observed: endless `IN 0x3E / OUT 0x3E` on `+0x8` after error 27).

### 5d. IRQ detection and conflict logic (`SETVFX1.EXE` symbol names)

Exported-by-string function list (exact names in the binary):
`_VIPDetect _VIPReset _VIP_Test_Port _VIP_Test_IRQ _VIP_Test_Both
_VIPTestIRQ _VIPTestInterrupt _VIPIRQInit _VIPIRQSetting _VIPIRQStatus
_VIPHookIRQ _VIPUnHookIRQ _VIPGetVector _GetPort _GetIRQ _VipportSet
_Vipport _SetupAddresses _GetFreeAddress _Accept_VIP _Cancel_VIP
_goodPort _badPort _goodIRQ _badIRQ _CheckUltrasndIRQ _CheckBlasterIRQ
_CheckMouseIRQ _VIPAbusInit _VIPAbusSETInit _VIPAbusControl _VIPAbuschksum`
(sources: `setvip.c, vipabus.c, vipio.c, newvipio.c, support.c`).

Behavioral notes: IRQ choice is user/menu-driven, then *tested*; the three
`Check*IRQ` helpers compare the candidate against detected SoundBlaster,
Ultrasound (`_dddgravis_port`) and mouse IRQ usage and surface `CONFLICT`.
Hook/unhook installs a temporary handler (`_VIPInterrupt`,
`_oldvipinterrupt`, `_irqtestflag`) for the live interrupt test. The
`TEST RESULTS REPORT` distinguishes `TEST FAILED` (card didn't answer) from
`CONFLICT` (card answered but IRQ unusable) from `DETECT` (good).

Disassembled IRQ-test flow (`SETVFX1.EXE @0x46CB`, helpers at `0x422D`,
`0x4497`, `0x5B09+`):
pre-gates first — `0x45A6` parses the `BLASTER` string for `I<n>` and fails
the whole test (code 2) if it equals the requested IRQ; `0x44C5` does the
same against the comma-separated `ULTRASND` string (code 3); `0x461F`
probes INT 33h SENSE plus a key-wait (code 4). Only then: hook the IRQ
vector (INT 21h AH=25h), program IRQCFG twice via `0x422D` (combined byte
`(irq & 0x7F) | (mode<<4)`, mode 1 then 0, with delays), and spin max 100
iterations reading `+0xF` while waiting for `_irqtestflag` to become
**exactly 1**. The card itself is expected to raise the IRQ as a result of
the IRQCFG programming — that edge is the entire trigger; without it the
flag stays 0 and the verdict is fail. The ISR (`@0x4497`) only reads
`base+0x0`, bumps the flag, and EOIs the master (or both PICs for IRQ > 7).
PIC mask management is all DX-based read-modify-write (`in/out dx,al` on
`0x21/0xA1`, never immediate `E6 21`): hook clears the line's bit, unhook
restores the saved mask, and every ISR path EOIs both PICs where needed.
Neither `SETVFX1.EXE` nor `VFX1.COM` assumes an already-unmasked line —
they manage it — but both assume the programmed IRQCFG write produces
exactly one delivered interrupt.

Addressing convention for §5e–§5g: offsets are runtime
segment-relative as disassembled (for `.COM` files, runtime = file +
`0x100`); resident/transient segment bases vary per boot.

### 5e. Bus RX/TX primitives (both binaries, same shapes)

Receiver (`VFX1.COM @0x2882`; `SETVFX1.EXE` same code at its segment
offset `0x04`; args base / buffer / count):

```
di = base+8, [bp-2] = base+9, si = 0
IN +0x8 -> cl
if (cl & 0x04): return 0                    ; EMPTY at entry: nothing to do
OUT +0x8, (cl & 0xB7) | 0x01                ; entry shape
loop:
  IN +0x9 -> buf[si++]                      ; (@0x28B5 / @0x41)
  if (si == count): si = count - 1          ; clamp: overwrite last slot
  IN +0x8 -> cl                             ; (@0x28C9 / @0x58)
  if (cl & 0x20): continue loop             ; data-available: read more
  if (!(cl & 0x04)): poll +0x8 again        ; drained but not complete
  OUT +0x8, cl | 0x48 ; return si           ; exit shape, transfer done
```

So `+0x8` bit `0x20` = data-available (1 while the VIP holds a byte)
and bit 2 (`0x04`) = EMPTY/complete (1 when dry). A transfer always
terminates on drain (no until-dry infinite loop) as long as those two
bits honestly track the FIFO; holding `0x20` set with bit 2 clear is
what a stuck receiver loop looks like on a log (hundreds of thousands
of `+0x9` reads, zero `+0x8` writes).

Sender (`VFX1.COM @0x28EF`-family; `SETVFX1.EXE` at segment offset
`0x83`): length = `([buf+2] & 0x7F) + 4`; head
`OUT +0x8, (cl & 0xBE) | 0x08`; each byte sent only while `+0x8`
bit `0x10` is set; any `+0x8` read with bit `0x02` set aborts with
return 0 (caller retries); exit shape `OUT +0x8, cl | 0x48`
(`VFX1.COM`) / `cl | 0x41` (`SETVFX1.EXE`).

### 5f. Demuxer + packet dispatcher (`VFX1.COM`)

Demuxer `@0x23AA`: reads `0x84` bytes per receiver call into the
buffer at `[0x1C0]+0xA5E`, scans for `0x50`-led packets with length
`([b+2] & 0x7F) + 4`, and runs each candidate through the checksum
helper `@0x1CEF` — which XORs bytes `[0..count]`
(`count = ([b+2] & 0x7F) + 3`) and *stores* the result at `[count]`
(it is a checksum writer, also used when sending).

Dispatcher `@0x2232` (per filed packet at `si`):

- `[si+1]==0x6E && [si+3]==0xE0` → `ATTENTION`: sets `[0xA4A]`
  (after zeroing `[si+1]`). The orchestrator waits on `[0xA4A]`
  before sending `IDREQ`.
- `[si+3]==0xE1` → `ID reply` path (`@0x2271`: checks `[0x804]`,
  copies `0x22` bytes, hands to the sender layer).
- `[si+1]==0x51 && [si+3]==0xC2` → status branch (presence/power
  replies): rewrites `[si+1] = 1`, sets `[0x4F3] = 1`, then decodes
  capability bits — `[si+5]&4` with `[si+4]&4` sets `[0x1AD] = 1`
  (reply valid; else `[0x1AD] = 0`), `[si+5]&1` stores `[0x805]`,
  further bits set `[0x4F6]/[0x4F7]/[0xAEA]/[0xAEB]/[0x83E]/[0x4F2]`.
- `[si+1] >= 6` (e.g. default `0x6E`) is rewritten to `3`.
- Every packet is copied (up to `0x42` bytes) to the per-device slot
  `[src + 0xAEC]`, with freshness flags `[src + 0xAE4] = 1` and
  `[src + 0x838] = 1`. Tracking reports (`src 02/04`, cmd `0x08`)
  therefore land at `[0xAEC+0x84]` (HMD) and `[0xAEC+0x108]`
  (puck) — the slots a driver-level consumer reads.
- Anything else falls through ignored (tracking included, for init
  purposes).

### 5g. VIP ISR, orchestrator, presence probes (`VFX1.COM`)

- ISR at segment offset `0x279A` (observed hooked at INT `0x0D` for
  IRQ 5): pushes regs, calls the demuxer `@0x23AA`, EOIs both PICs
  (`OUT 0xA0/0x20, 0x20`), and — guarded by `[0x1A7]` (re-entrancy)
  and `[0x1A4]` — runs the orchestrator `@0x2474` on a private
  stack (`SS:SP` swapped, `STI`, restored after). No callers exist:
  it is entered only via the interrupt. Enumeration is therefore
  IRQ-driven by design; a masked line stalls everything past `IDREQ`.
- Presence probe `@0x1F62(base, addr)`: builds
  `addr 50 82 F7 00` (presence poll, pad `0x00`, no checksum),
  sends it via `@0x1DC1` (retry-while-zero), then spins up to
  `0x7530` iterations (`IN base+0` filler) waiting for `[0xAE5]`
  (set when a reply is filed) with `[0x1AD]==1` (reply valid for
  this probe): returns 1 found / 0 not-found.
- Collector `@0x1FF6` probes addrs `2, 4` (`[bp-2] = 2; +2 while
  < 6`), records discovery in `[0x1BC]/[0x1BE]`; when its return
  matches `[0x1BC]` the orchestrator proceeds to power
  (`@0x1F2D/@0x1F49`) and assign (`@0x1DF1`).
- TSR data lives in a DOS-allocated segment whose number the init
  stores at `cs:[0x103]` (`mov [0x103],dx` + `mov ds,dx` at file
  `0xC10`; the zero-fill `rep stosb` covers `0x3DA…0xD08`,
  including the slot/flag ranges above). The INT 33h dispatcher
  compares `cs:[0x107]` against function codes
  (`3,4,5,9,D,E,12,10,101,100,10D…155,13,3A…`, file `0xC47+`) and
  funnels every function through a shared worker (`@0x1226` →
  `@0x11B5/@0x111C/@0x0EA1`, with a per-function control-byte table
  at `[0x11A]/[0x134]/[0x14E]` written to the VIP ports).
- `SETVFX1.EXE` test-side flow for comparison: sample loop at file
  `0x7D12` (receiver `0x84`/call, `or ax,ax; jne` retry), a
  `3500× IN [0x10C0]` drain (`@0x6A83`; `[0x10c0]` holds the base,
  so this reads `+0x0`), a send-then-`30000×` poll sequencer
  (`@0x6AE3`: sender, then `IN [0x10c0]` filler while waiting for
  the per-device flag `[idx+0xA2E]` set at file `0x766B`,
  timeout `0x7530`), and keyboard input via INT 16h + port `0x60`
  (no gameport use anywhere). Both TEST menus are keyboard-driven
  and sample nothing once displayed.

## 6. ACCESS.bus layer (the card is found first, the helmet second)

From `serial communication protocol.doc`,
`serial communication protocoll.doc`, `TrackingInitialization.doc`,
`connectors VFX1.doc`, `VFX1.H`:

- Physical: I2C-family two-wire bus (SCL/SDA + GND, +5 V optional). VIP is
  bus host at **7-bit address `0x50` (`01010000`)** — the "General
  ACCESS.bus Host" address. HMD powers up on **default address `0x6E`**.
- Bring-up order (Rob Smith's capture, all counts HEX):
  1. Helmet plugged in → sends `ATTENTION` packet (host addr, default addr,
     control msg 1–2 bytes, attention function, null pad, checksum).
     Repeats until ACKed.
  2. Host sends `ID request` to default address → every device there
     replies with `identification reply` (`0x1D`-byte control + `B` protocol
     rev + `V1.2___`-style firmware + `FORTE___` vendor + `VFX1101_`/`PUCK102_`
     name + 4-byte serial + checksum).
  3. Host sends `assign-address` (echo of ID data + new address byte).
  4. Optional `capabilities request/reply` (`VFX1HT` for HMD; puck reports
     buttons/axes). Skippable.
  5. HMD only: `power-management control` (mode 0 = Normal → LCD backlights
     on, `POWER CONTROL reply` expected). Puck has no power management.
  6. `enable-application-reporting` (1 = on) per device → streaming starts.
  7. Steady state: HMD is master-transmitter, VIP slave-receiver; HMD sends
     even with no ACK. Host polls presence with command `0xF7`.
- Tracking packet (HMD → VIP), 12 bytes:
  `[VIP addr 0x50][HMD src][protocol/len 0x08][cosHi][cosLo][sinHi][sinLo]
   [pitchHi][pitchLo][rollHi][rollLo][XOR checksum]`.
  Extraction: `value = high*256+low`, DC-correct `(max+min)/2`, normalize,
  best-fit line (see `Linkbox.xls` reference — not in this tree).
- Wire formats observed live (log-verified, checksummed, framed by a
  `+0x8` write within 8 `+0x9` writes — these corroborate, and in one
  case complete, the paper captures above):
  - `ATTENTION`: `50 6E 82 E0 00 5C`.
  - `ID request` (broadcast): `6E 50 81 F1 4E`; directed (tests and
    the TSR's own probing): `<addr> 50 81 F1 <xor>`.
  - Identification reply: `50 <addr> 9D E1 42 fwver(7) FORTE___ name(8)
    serial(4) xor` (33 bytes on the wire; `<addr>` is the device's
    current address, `0x6E` until assigned).
  - `enable-application-reporting`: `<addr> 50 82 F5 00 <xor>`.
  - Presence poll: `<addr> 50 82 F7 00` (+ pad, polled by `@0x1F62`).
  - `assign-address` (34 bytes; a 33-byte variant is also accepted):
    `6E 50 9E F2 42 fw(7) FORTE___ name(8) serial(4) addr xor`.
  - Status/presence/power reply accepted by the dispatcher:
    `50 51 LL C2 b4 b5 … xor` with `[4]&4` and `[5]&4` set.
  - First observed assignment in the wild: HMD → 6 (later also
    puck → 6 in a session where the HMD stayed unassigned).
- `VFX1.H` maps this onto driver packets: `SYSTEM_CPKT` (contains 4-char
  `vipid`), `VENDOR_CPKT` (`Forte Technologies, Inc.` / `VFX1 CyberPuck`,
  serial/revision fields), `HMS/HTD/HPD` config packets, `SVM` video-mode
  packets; IDs `VFX1 / VFXZ / PUCK102`; device classes `DCHMS 2, DCHTD 3,
  DCHPD 4`.

## 7. Driver-level "detection" (INT 33h SENSE API — not the ISA probe)

From `DOSSDK/QSTART|EXAMPLE1|EXAMPLE2` (`*.C`, `*.H`, `DPMI.*`):

- Vector `0x33`. Finding the driver = scanning function masks
  `MIN 0x60 … MAX 0x70`: set `AX = mask<<8 | SENSE_DRIVER_STATUS (0x7F)`,
  `INT 33h`, expect `AX` echoed back. Nonzero mask = driver present; this
  says nothing about hardware by itself.
- Then: `GET_CONFIGURATION (0x03)` → walk packets to `NULLP` terminator,
  match vendor/product strings; `SET_CONFIGURATION (0x04)` to enable
  (`den` bits); `REPORT (0x05)` to stream; `GET_DEVICE_DATA (0x70)` /
  `SET_DEVICE_DATA (0x71)` fast path; `SET_VIDEO_PAGE (0x80)`,
  `SET_ZERO/RESET_ZERO (0x08/0x07)`, `TEST_DEVICES (0x09)`.
- 32-bit/DPMI builds (`EXAMPLE2` + `DPMI.C`) do the same via simulated
  real-mode INT (`DPMI 0x31 AX=0x0300 → INT 33h`) with a shared low-memory
  config/report buffer (`allocDOS/freeDOS`, `VFX1MouseInt`).
- Practical consequence: an app can see "driver loaded, zero devices" when
  the ISA probe failed but the TSR stayed resident — always check packet
  contents (SYSI/VNDI/HMS/HTD/HPD), not just the mask.

## 8. Failure signatures (exact strings to grep for)

- `VIP Card base port not detected.  Check VIPPORT setting.` (`VFX1.COM`)
- `VIP Host card not configured. (Unloading VFX1)` (`VFX1.COM`)
- `VIPPORT Environment string not setup, using default.` (`VFX1DIAG.EXE`)
- `Error#(%d) getting|enabling configuration packets / during reporting /
  setting mono|stereo video mode` (`VFX1DIAG.EXE`)
- `TEST FAILED / CONFLICT / DETECT`, `ERROR:001/002/003` autoexec errors
  (`SETVFX1.EXE`)
- `ERROR READING|DOWNLOADING VFX1 DATA`, `Puck is on address: / VFX1 is on
  address:` (bus layer, after ISA detection passed)
- `Error 27` from `VFX1.COM`: the idle check at file `0x4806`
  (`EMPTY-SET` wait, one `+0x9` read, `EMPTY-CLEAR` wait) requires the
  byte to be `0xFF` — any bus traffic visible to an unframed read
  trips it into a retry loop.

## 9. File-by-file inventory (what each file contributed)

Root: `README.TXT` — SDK release notes (v2.13, 2/26/96), DOS vs Win95 SDK
layout, sample-code map, DirectInput notes. Installers: `VFX1SDK.EXE`
(DOS/Win installer), `vfx1_213.exe` (flat SDK archive).
`VFX1/`: `VFX1.COM` (TSR: ISA probe + ACCESS.bus init + INT 33h driver;
all §5b messages); `SETVFX1.EXE` (setup/test UI; §5a/§5d + F1–F4/button and
report strings); `VFX1DIAG.EXE` (INT 33h + VESA display-path diagnostics);
`VRMOUSE.COM` (keyboard/mouse emulator; no probing); `VFX1.INI` (video-mode
→ input/output routing table, e.g. `M13 I06 O02`); `FIELDPRG.EXE/.INI`
(tracking calibration UI text); `IFCAL.EXE` (factory cal tool, no strings
of detection interest); `VRMOUSE.INI` + `VRMINI.TXT` (emulator config
language); `VFX1_12.FAQ` (product FAQ: VIP = ACCESS.bus controller + video
buffer, ISA-slot/VESA-cable requirements, card/game lists);
`README.TXT/.BAT` (driver placement, `VIPPORT=2x0 Iy`); `VFX1.F1/.F2/.F3/.F11`
(DSP firmware images downloaded post-detection); `VFX1.3D/CYBRPUCK.3D/
SPACE.PCX/.COL/FOCUS.PCX/CITIES.DAT/VFXDECAL.TM` (demo/optics assets);
`VR*.BAT` (per-game `VRMOUSE <game> → game → VRMOUSE -F` wrappers).
`VFX1/DOSSDK/`: `QSTART/{QSTART.C,VFX1REGS.C/.H,VFX1.H}` (minimal SENSE
client + register/bit map incl. `ABUS_CONTROL` bits and `VIP_IRQCFG`);
`EXAMPLE1/{EXAMPLE1.C,VFX1.H}` (config-packet walk client);
`EXAMPLE2/{EXAMPLE2.C,VFX1.H,DPMI.C/.H,MAKEFILE,EXAMPWAT.BAT}` (same via
DPMI/Win; Borland `bcc -ml` / Watcom `wcl386` build lines).
`doc/`: `VFX1manual.doc` (jumper, slot, `SETUP VIPPORT`, AUTOEXEC,
troubleshooting — §2–§4); `connectors VFX1.doc` (VESA/HD26/Molex pinouts,
reversed-cable warning); `serial communication protocol.doc` +
`serial communication protocoll.doc` (addresses, 12-byte packet, master/
slave roles); `TrackingInitialization.doc` (ATTENTION→ID→assign→power→
reporting with byte counts); `HowtokeepaliveyourVFX1.doc` (2004-era
ISA-less survival: 2nd PC / composite mod / linkbox — context only);
`VFX1 Display Schematics.doc`, `VFX1LcdBrdChanges.doc` (video-path HW,
no detection role); photos/`*.jpg` (boards, signals, notes — visual
reference); PDFs (`Access Bus 3`, `App Report Timings`, compass/tracker
sensor papers, `TMS320C2x`, `IR3Y15`) — bus/tracker background.
`doc/doc/`: `VFX1manual.doc` (duplicate of User's Manual);
`i2c-01..03.txt` (I2C FAQ 1.6: START/STOP/ACK, arbitration, pull-ups —
general background for the ACCESS.busAdventure); `I2C_LOW.ASM/TESTI2C.ASM`
(PIC I2C bit-bang + slave-presence ping via ACK — pattern reference, not
VIP code); `i2c-code.doc` (one-line stub); `I2C.Zip` (archive of same).

## 10. Claim → source map (for verification)

- Four bases / four F-keys / port table bytes → `SETVFX1.EXE` strings
  (`F1 260…F4 2C0`, `_BUT260…`), file offsets `0x59AE`, `0x3C447…`;
  `VFX1/README.TXT` (`2x0`); Manual ch. 3 (jumper).
- IRQ set {5,7,10,11,12,15} → `VFX1/README.TXT`; conflict helpers →
  `SETVFX1.EXE` (`_CheckBlasterIRQ…`, `_VIPIRQ*`).
- Echo `0x0A/0x05` + `HWID==0x0D` + `!=0xFF` + `CFGSTATUS&6==6` →
  disassembly §5a–§5c (offsets above); constants `0x0C/0x0D/0x0E/0x0F/0x0A`
  → `VFX1.H`.
- `VIPPORT` format/defaults/AUTOEXEC order → `VFX1/README.TXT`, Manual
  App. A, `SETVFX1.EXE` (`260 I7`, `@SET VIPPORT=`), `VFX1.COM`
  (`VIPPORT=`, `-p#`).
- INT 33h mask scan + packet API → `VFX1REGS.C`, `EXAMPLE1.C`, `EXAMPLE2.C`,
  `VFX1.H` (`SENSE_VECTOR 0x33`, `MIN/MAX_SENSE_FUNCTION`,
  `SENSE_DRIVER_STATUS 0x7F`).
- ACCESS.bus addresses/packets → `serial…doc` (`VIP 01010000`, `HMD
  00000010`/default `0x6E`), `TrackingInitialization.doc` (full sequence),
  `VFX1.H` (`VFX1ID/VIP…`, packet structs).
- RX/TX shapes, demuxer, dispatcher branches, ISR, orchestrator,
  probes, TSR data/dispatch layout, SETVFX1 sample/drain/sequencer
  loops, observed wire formats → disassembly of `VFX1.COM` /
  `SETVFX1.EXE` (§5e–§5g, §6 wire list; runtime offsets as printed).
- RX/TX shapes, demuxer, dispatcher branches, ISR, orchestrator,
  probes, TSR data/dispatch layout, SETVFX1 sample/drain/sequencer
  loops, observed wire formats → disassembly of `VFX1.COM` /
  `SETVFX1.EXE` (§5e–§5g, §6 wire list; runtime offsets as printed).

## 11. Emulator working values (NOT SDK claims)

The packet bytes below are not in the SDK materials; they are the
synthesis an emulator developed to satisfy the disassembled checks
above, verified by the firmware accepting them (tests passing,
assigns completing). Recorded here so a regression can be diffed,
not as hardware documentation:

- `ATTENTION` as sent: `50 6E 82 E0 00 5C`.
- HMD tracking: `50 02 08` + cos/sin yaw pair (±8192 around mid-scale
  `0x8000`) + pitch/roll (±1024 around `0x8000`) + XOR. Raw axis
  bytes are unsigned DC-biased: signed zeros read as pegged extremes
  (`BEYOND TRACKING RANGE`); everything must be centered on `0x8000`.
- Puck tracking: `50 04 08` + centered yaw (`0x8000`) + pitch/roll
  sweeps + buttons byte 9 (`bit0=top, bit1=middle, bit2=bottom`,
  assumed row order, unverified) + pad `0x00` + XOR.
- ID reply: `50 <addr> 9D E1 42` + `V1.2___`/`V1.4___`,
  `FORTE___`, `VFX1101_`/`PUCK102_`, 4-byte serials + XOR.
- Status reply (POLL/power): `50 51 82 C2 04 04` + XOR.
- `+0x8` status model that keeps the receiver honest: bit `0x20` =
  data-available (queue pending), bit 2 = EMPTY/complete (queue dry);
  legacy idle bits otherwise untouched (the `0x4806` check and the
  `0875` pulse loop depend on exact values).
- Observed guest PIC discipline (constrains any IRQ emulation): the
  guest masks its VIP IRQ ~1.5 s into boot (after its own IRQ test)
  and never re-opens it; enumeration past `IDREQ` runs inside its
  VIP ISR, so a silent line stalls it even with perfect bytes.
