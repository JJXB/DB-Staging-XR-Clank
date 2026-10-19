# SETVFX1.EXE — Full Disassembly Infodump

Source binary: `D:\Emu\DOS\VFX1\setvfx1.exe` (**269,286 bytes = `0x41BE6`**,
modified 1997-01-20 22:11:18). Toolchain: Rizin
(`C:\Apps\rizin-win-installer-vs2019_static-64\bin\rizin.exe` +
`rz-bin.exe`) plus a Python/Capstone-16 recursive pass (472 starts: 286
`55 8B EC` prologues + `E8` targets + entry `0x1800`). Scope: **the setup /
VIP test program itself** — MZ layout, strings, INT/port census, routine
map, baseport→IRQ→reporting-sweep→directed-IDREQ→`AUTOEXEC.BAT` flow. The
resident driver it configures lives in `vfx1_drv.md`; card background lives
in `VFX1.md`. Nothing below comes from any emulator source.

All offsets are **file offsets (hex)** unless marked vaddr. Rizin
`vaddr = file − 0x1800` (header `0x1800` bytes). Offsets in §6 give both as
`file/vaddr`.

## 1. The 30-second version

1. `SETVFX1.EXE` is a **Borland MZ EXE** (not MSC): `Borland C++ -
   Copyright 1991 Borland Intl.` at file `0x3B1F4`, Borland per-function
   `DS`-reload prologues, `C8/C9` enter/leave, `lcall` A-Bus dispatcher,
   trailing source names (`setvfx1.c/SETVFX1`, `setvip.c/SETVIP`,
   `vipabus.c/VIPABUS`, `vipio.c/VIPIO`, `newvipio.c/NEWVIPIO`,
   `support.c/SUPPORT` at file `0x41AFF–0x41BC6`).
2. CLI (`usage:` block at file `0x2CE72–0x2D177`): bare/`-`/`/` args —
   `h/H/?` help; `1` = SET VIPPORT subroutine only; `2` = SET FOCUS;
   `3` = Test VFX1; `4` = Test CYBERPUCK; `5` = CALIBRATOR; `l#/L#`
   language (E English default / G German / F French / S Spanish / I
   Italian / R Russian). Arg matching uses `_vip_strncmp`, not a single
   `CMP AL,'h'`.
3. The setup flow is: read `VIPPORT` via a PSP env-scan → probe bases
   `260/280/2A0/2C0` (candidate table bytes at file `0x59AE`: `60 02 80
   02 A0 02 C0 02`, F1–F4 buttons) with echo `+0x0` + status-bit-`20h`
   drain → HWID `+0xC/D/E` + CFGSTATUS `+0xF` → IRQ conflict test
   (`_CheckBlasterIRQ` / `_CheckUltrasndIRQ` / `_CheckMouseIRQ`: read
   `ULTRASND=` / `BLASTER=` / `VRMOUSE` env, compare against candidate
   `5/7/10/11/12/15`, F5–F10 buttons; PIC `0x21/0xA1` mask test in
   `VIPTestIRQ`) → REPORTING sweep → directed `02 50 81 F1`-IDREQ discovery
   (`VFX1 is on/PUCK is on address: $` vs `OFFLINE`) → `TEST RESULTS
   REPORT` (`GOOD/BUENO`, `BASEPORT/INTERRUPT/VFX1INIT`, `TEST PASSED /
   TEST FAILED / CONFLICT … WITH ANOTHER / MAYBE IN`, `JUMPERS`) → `DO YOU
   WANT THE AUTOEXEC.BAT MODIFIED` → `@SET VIPPORT=<base> I<irq>`
   (`MODIFICATION WAS SUCCESSFUL/UNSUCCESSFUL`, backup `autoexec.01`,
   `ERROR:001/002/003`).
4. The two bus loops that matter for emulation are the **drain-until-dry
   flush** (`Device_Test`, file `0x7C94`: `CALL 7295`, `CLI`, `lcall`,
   `+9 IN`, `+8 IN`, `TEST AX,20h`, `JNE drain`, `lcall 041B:0047 send` —
   the `RX 0x84 / or ax,ax / jne retry` shape that only exits on dry-0)
   and the **REPORTING sweep sender** (file `0x77DF`: `MOV SI,2 …
   ADD SI,2; CMP SI,0Ch` over candidates 2,4,6,8,10 with the
   `02 50 83 C1` template send plus 1000s-class `+0x0` polls of the ISR
   flags `[5A]/[5C]`).
5. A trailing overlay (`file 0x3C6F0 / 0x54F6` bytes, rizin vaddr
   `0x100000`) holds sprites/3D/PCX, not code — Capstone-16 finds zero
   `in/out/int` there. `seg_011 (0xF0E0)` + `seg_012 (0xFB00)` hold
   fonts/tables/palette ramps (ASCII tables file `0x29940–0x2A840`,
   gradient ramps `0x35017–0x364xx`).

## 2. MZ header / segment layout

Raw MZ (`struct <14H`, file `0x00–0x1B`):

| Field | Value |
|---|---|
| sig | `0x5A4D MZ` |
| lastsize | `0x00F0` (240) |
| nblocks | `0x01E4` (484) |
| nreloc | `0x058D` (1421) |
| hdrpara | `0x0180` (384) = header `0x1800` bytes |
| minpara | `0x0000` |
| maxpara | `0xFFFF` |
| SS | `0x3AE7` |
| SP | `0x0080` (128) |
| cksum | `0x0000` |
| IP | `0x0000` |
| CS | `0x0000` |
| relocoff | `0x003E` (62) |
| overlay# | `0x0000` |

`rz-bin -I`: `arch x86, bits 16, bintype mz, class MZ, machine i386,
os DOS, subsys DOS, binsz 0x41BE6, static true, havecode true, stripped
false`. Load image = `0x41BE6 − 0x1800 = 0x403E6`. Entry `CS:IP
0000:0000`, `SS:SP 3AE7:0080` (PSP-relative; CS/IP fixed by relocs).
Reloc table at file `0x3E`: 1421 `SET_16` entries (first:
`0000:0001, 0000:005E, 0000:00A3, 0000:013D, 0379:03F9, …`).
`rz-bin -s/-E` empty (no COFF/PE symbols/exports); `-e` reports
`vaddr 0x0 / paddr 0x1800 type program`.

`rz-bin -S` (`paddr/size`, `vaddr = paddr−0x1800`): `seg_000
0x1800/0x3790`; `seg_001 0x4F90/0xA20`; `seg_002 0x59B0/0x7E0`; `seg_003
0x6190/0x720`; `seg_004 0x68B0/0xA0`; `seg_005 0x6950/0x1690`; `seg_006
0x7FE0/0x100`; `seg_007 0x80E0/0x570`; `seg_008 0x8650/0x550`; `seg_009
0x8BA0/0x2980`; `seg_010 0xB520/0x18E0`; `seg_011 0xCE00/0xF0E0`;
`seg_012 0x1BEE0/0xFB00`; `seg_013 0x2B9E0/0x360`; `seg_014
0x2BD40/0x70`; `seg_015 0x2BDB0/0x10C0`; `seg_016 0x2CE70/0x320`;
`seg_017 0x2D190/0x2070`; `seg_018 0x2F200/0x35E0`; `seg_019–027`
small `0xA0–0x130` (`0x327E0–0x32EA0`); `seg_028 0x32FD0/0x3400`;
`seg_029 0x363D0/0x4E20`; `seg_030 0x3B1F0/0x1480`; `seg_031
0x3C670/0x80`; extra `vaddr 0x3AEF0/0x10`; overlay file `0x3C6F0/0x54F6`
(vaddr `0x100000`).

Interpretation: single MZ module + trailing data overlay. Code in
`seg_000–010` (286 `55 8B EC` prologues found). Strings/UI in
`seg_015/016/017/018/028`. Stack tiny on load (`SP 0x80`); Borland
startup resizes via `INT 21h AH=4Ah`, saves `SS/SP` to
`_user_ss/_user_sp`, switches to `_my_stack` (symbols file
`0x40E9B/0x40EA5/0x40EAE`). Heap: `_heapbase` (file `0x41A08`),
`_dddFreeBaseMemory`, EMS check (`INT 67h`, `This program requires an
EMS handler`, `Not enough EMS available`).

Rizin limits: `aa/afl` collapses to `entry0 vaddr 0x0 size 700 (423
fcns)`; `iz/is/ie/iS` return empty tables for MZ. String/function truth
below is the Capstone pass, manually verified.

## 3. Meaningful string table (file offsets)

4-char noise and `83 CD`-as-INT false hits omitted. `0x40`-stride menu
blocks carry EN + DE/FR/ES/IT/RU tokens (only EN quoted here).

Banner/env (file `0x2BCD2–0x2BE51`): `0x2BCD2 VIPPORT`; `0x2BCDA
SETVFX1 Copyright 1995 FORTE Technologies Inc.`; `0x2BD0A ' Version
1.20a'`; `0x2BD20 vfx1.3d`; `0x2BD2E cybrpuck.3d`; `0x2BD3A -  SET VFX1
MENU -`; `0x2BD62 ULTRASND`; `0x2BD6B BLASTER`; `0x2BD73 Forte Tech`;
`0x2BD7E VRMOUSE`; `0x2BD8C/0x2BDA6 vfx1.com`; `0x2BD95/0x2BDAF -p%x`;
`0x2BDBA VFX1`; `0x2BDBF PUCK102`; `0x2BE12 Puck is on address: $`;
`0x2BE28 Puck is OFFLINE`; `0x2BE3B VFX1 is on address: $`; `0x2BE51
VFX1 is OFFLINE`.

A-Bus packet templates (file `0x2BDE6–0x2BE0A`, binary, runtime-patched):
`6E 50 81 F0 00 | 6E 50 82 C0 00 00 | 02 50 83 C1 00 00 00 | 6E 50 81
F1 00 | 02 50 82 F5 01 00 | 02 50 81 B1 00 | 02 50 82 F6 01 00` —
`02 50 83 C1` = sweep sender, `02 50 81 F1 / 02 50 82 Fx` = directed
IDREQ/discovery family (the observed `02 50 81 F1 22` is this template
with addr/checksum patched).

CLI help (file `0x2CE72–0x2D177`): `usage:`, `setvfx1
(_-/)[12345hH?lL]# ...`, `setvfx1:`, `The useable arguments can be just
passed, or passed with / a '-' or '/' preceeding them… 'h','H','?'`,
`1 This will activate the Set VIPPORT subroutine only.`, `2 …Set
FOCUS…`, `3 …Test VFX1…`, `4 …Test CYBERPUCK…`, `5 …CALIBRATOR…`,
`l#, or L# This is the language choice… E English (default) / G German
/ F French / S Spanish / I Italian / R Russian`.

AUTOEXEC block (file `0x2F1A8–0x2F1F1`): `VIPPORT`, `SET`, `@SET`,
`VIPPORT`, `VIPPORT`, `260 I7` (default `VIPPORT=260 I7`), ` I`,
` \r\n`, `autoexec.01` (backup), `autoexec.bat`, `@SET VIPPORT=`.
Fallback paths `0x327AD c:\autoexec.bat`, `0x327C1 d:\autoexec.bat`;
spawn `%s%s ifcal.exe / fieldprg.exe`,
`%s%s space.col/space.pcx/focus.pcx`, `fieldprg.exe`.

Main menu (file `0x2F200–0x2F8xx`): `F1   SETUP VIPPORT`, `F2    SETUP
OPTICS`, `F3       TEST VFX1`, `F4  TEST CYBERPUCK`, `F5
CALIBRATION`, `ESC           EXIT`, `F6     SOFTWARE BY`, `ARE YOU
SURE`, `INSTALL VERSION 2.13`, `SOFTWARE BY: FORTE`, `GRAPHICS BY:
VIRTEK`.

Report tokens (file `0x2FC50–0x31C58`): `TEST RESULTS REPORT`,
`GOOD/BUENO`, `BASEPORT`, `INTERRUPT`, `VFX1INIT`, `TEST PASSED`, `TEST
FAILED`, `UNABLE TO`, `DEVICE`, `WITH ANOTHER (+FR AVEC UNE AUTRE)`,
`CONFLICT`, `MAYBE IN (+FR PEUT-ETRE EN)`, `PRESENT CHOICE`,
`INITIALIZE`, `DETECT`, `VIP CARD`, `CANNOT PERFORM`, `TEST`,
`JUMPERS`, `IRQ` (file `0x30F00`), `A   ACCEPT`, `C   CANCEL`,
`T     TEST`, `DO YOU WANT THE AUTOEXEC.BAT MODIFIED`, `MODIFY
AUTOEXEC.BAT REPORT`, `MODIFICATION WAS SUCCESSFUL/UNSUCCESSFUL`,
`ERROR:001 - COULD NOT OPEN AUTOEXEC.BAT (0x31690)`, `ERROR:002 -
COULD NOT OPEN BACKUP FILE (0x31780)`, `ERROR:003 - DISK FULL
(0x31870)`, `AUTOEXEC.BAT SAVED AS `, `BACK`, `CANCEL`.

Base/IRQ pick (file `0x3256F–0x325AE`): `F1 260`, `F2 280`, `F3 2A0`,
`F4 2C0`, `F5   5`, `F6   7`, `F7  10`, `F8  11`, `F9  12`, `F10 15`.

Calibrator/test (file `0x325B5–0x32770`): `PICK METHOD`, `BY COUNTRY
AND CITY`, `MANUAL CALIBRATION`, `CANCEL`, `CONSULT THE
TROUBLESHOOTING / SECTION OF THE MANUAL`, `SPACE       CENTER`,
`TOP     RIGHT VIEW`, `MIDDLE   REAR VIEW`, `BOTTOM   LEFT VIEW`,
`ERROR READING VFX1 DATA`, `HEADSET CORRUPTED`, `ERROR DOWNLOADING
VFX1 DATA`, `PLEASE WAIT`, `VFX1 NOT ONLINE`, `BEYOND TRACKING RANGE`,
`- TEST VFX1 MENU -`, `CYBER PUCK NOT ONLINE`, `- TEST PUCK MENU -`.

Conflict sources: no `SOUND/CREATIVE/SB16/GUS` literals — only env
names `ULTRASND`, `BLASTER`, `VRMOUSE` + code symbols
`_CheckUltrasndIRQ` (file `0x3EFEC`), `_CheckBlasterIRQ` (`0x3EFFE`),
`_CheckMouseIRQ` (`0x3F00F`). The test reads `ULTRASND=/BLASTER=/
VRMOUSE` env to derive the conflicting IRQ and composes the
`CONFLICT/WITH ANOTHER/MAYBE IN` sentence from tokens.
`** ERROR **  Device not yet supported. (0xBEBC)` is the only long
error outside the UI.

Symbol table (file `0x3EB2A–0x4144C`, `_`-prefixed C names): `_abort`,
`__IOERROR/__DOSERROR/__setupio`, `_Setvfx1_Graphics`,
`_check_for_vfx1`, `_VIPIRQInit/Setting/Status`,
`_VIPDetect/Reset/SetPalette/GetPalette/PageSelect/SetVideoMode/
TestInterrupt`, `_CheckUltrasndIRQ/CheckBlasterIRQ/CheckMouseIRQ`,
`_VIPTestIRQ/VIPTester/VIPAbuschksum/AbusControl/AbusSETInit/AbusInit`,
`_vip_strncmp`, `_ResetPuck/DownLoadVfx1/Device_Test`,
`_VIPGetVector/SetupAddresses/VIPInterrupt/VIPHookIRQ/VIPUnHookIRQ`,
`_GetPort/GetIRQ/VipportSet/ModifyAutoexec/Flushkeybd/Vipport/
Waitforkey/PleaseWait/done_testing/CyberPuck`,
`_VIP_Test_Port/VIP_Test_IRQ/Accept_VIP/Cancel_VIP/VIP_Test_Both/
goodPort/badPort/goodIRQ/badIRQ/cancel_Test/FollowMouse/ModifyBat`,
DDD/3DWARE (`DDDINITMOUSE/MOUSE…/INITKEYBD…`, `DDDFATALERROR`,
`ddd…`), globals (`_oldvipinterrupt/_irqtestflag/_viptestirq/
_viptestport/_vfx1id/_puckid/_irqflag/_vfx1setup/_vfx1address/
_puckaddress…`), UI (`_Setup_Vipport/_Setup_Focus/_Test_Vfx1/
_Test_CyberPuck/_Calibrator/_Report/_Good/_Bad/_Port/_Interrupt/
_VFX1Init/_Passed/_Failed/_Conflict/_Detect/_VIPCard/_Jumpers/
_ModifyReport/_Error1/2/3…`), buttons (`_BUT260/_BUT280/_BUT2A0/
_BUT2C0/_gPort/_gIRQ…`).

## 4. INT census (disassembly-confirmed)

A naive `CD xx` scan is polluted (`83 CD xx` = `OR BP,imm`, `80 BF CD
16` = `CMP [BX+16CD],imm`, font data). Below is the prologue + `E8`
recursive Capstone pass, deduped. File offsets; vaddr = file − `0x1800`.

| INT | File addrs | Use |
|---|---|---|
| `10h` | `0x11045, 0x11049` (+ `0x17121` VGA BIOS thunks) | Video/UI: `push di; mov ax,[bp+6]; push bp; INT 10` then `mov ah,0Fh; INT 10` (set/get video mode; `VIPSetVideoMode/PageSelect/SetPalette` also drive `0x3C7/0x3C9` below) |
| `16h` | `0x179F1, 0x179F9, 0x179FF, 0x17A03, 0x17A17, 0x17B34` | Keyboard: `mov ah,01h/00h; INT 16` poll/read, `AND AL,0Fh`, F1–F4/ESC dispatch, `Flushkeybd/Waitforkey` |
| `21h` | 85 uniq (e.g. `0x180A AH=30h`, `0x1D3B/44/4E/57…`, `0x2DAB AH=40h`, `0x35FB`, `0x3AF1`, `0x183BF AH=09h`, `0x17E5C/72`, `0x18452/68/8A/9F/CD/FF`, `0x1C0CD`, `0x5A22 AH=35h`) | DOS: `30h` version (startup `BA 9F39 … B4 30 CD21`), `09h` print `$` (usage/report), `35h/25h` get/set vector (`VIPGetVector/Hook/Unhook`), `48h/49h/4Ah` alloc/free/resize, `3Dh/3Eh/3Ch/3Fh/40h/44h` open/close/create/read/write/ioctl (`AUTOEXEC.BAT` + `vfx1.3d/cybrpuck.3d` download), `4Ch` exit, `0E/01/02` console, `43` attrib, `47/3B` dir; env via PSP `[2Ch]` scan for `VIPPORT=` |
| `33h` | `0x52A7, 0x52DE, 0x5305` | Mouse: DDD mouse + `FollowMouse`, `AX=2DA0h`-class init/show/hide/pos/button |
| `67h` | `0x17F30, 0x17F5C, 0x17F7C`, `0x1793/9F/AB/B7/C3`, `0x180E4` | EMS (`This program requires an EMS handler`) |
| `1Ah` | `0x18FC` (`B4 00 CD 1A`) | Time tick (delay seed) |
| `0Eh/22h` | `0xDD54` / one `0x22` | Rare; Borland overlay/exit glue, not UI |

False positives: `08h/10h/20h/40h/80h` at
`0x1EB99/1EBAE/1EBB7/1F091…/2B5E6…` are `83 CD/C3 xx`, not INT;
`CD 16` at `0x62F8/0xB47E`, `CD 2F` split at `0xA38x` (except one true
`INT 2F` at `0xA390` mux), `CD 13` at `0x16C44` (`02 CD`+`13 F5` split)
are data/dispersion false hits. `AH` breakdown near `CD 21` (lookback
`B4/B8`): `09` print, `25/35` vector, `30` version,
`3D/3E/3C/3F/40/44` file, `48/49/4A` mem, `4C` exit.

## 5. Port census (Capstone-confirmed)

Direct `imm8` (real code): `OUT 0x0F,AX (0x2B14)`, `OUT 0x43,AL /
IN AL,0x40 x2 (0x2BAB/2BB0/2BB7` PIT `43/40)`, `IN AL,0x64 x6
(0x1794F` kbd status), `IN AL,0x60 x2` (kbd data), `OUT 0x4F,AL x9`,
`OUT 0x20,AL x6` (PIC1 EOI/mask), `IN AX,0x0E x2`, plus VGA/PIT below.
537 further `None-DX` hits are `AL,imm`-style sequencer/data writes that
decode without `BA` in-window; many are really the `mov dx,bx + add
dx,off` VIP pattern (next row).

`DX`-indirect: `OUT DX,AL` 2342 decodes (145 uniq), `IN AL,DX` 626,
`OUT DX,AX` 268. Resolved `DX` (`BA` window + `ADD DX,imm` census:
`02 x47, 08 x5, 0F x5, 0A x2, 09/0C/0D/0E x1–2`):

- VIP card (base in `SI/BX/[0x10C0]/[BP+6]`, `mov dx,base; ADD
  DX,off`): `+0x0` echo/data, `+0x8` status, `+0x9` data, `+0xA`
  IRQCFG, `+0xC/D/E` HWID, `+0xF` CFGSTATUS. Examples: file `0x5A4E
  83 C2 0F EE` (`dx+0xF OUT 0`), `0x5A56 83 C2 0A EE` (`dx+0xA OUT
  IRQ bits`), `0x7CCC 83 C2 09 EC / 0x7CD2 83 C2 08 EC / A9 2000 75
  ED` (poll `+0x8` bit `0x20`), `0x7DB2 EE / 0x7DB8 EC` (`+0x0/+0x1`
  sequencer); `+0x0` echo/data (`0x6A91/0x7DB2`-class),
  `+0x8` status (`0x6926/0x6942/0x7CD2/0x7E60`-class),
  `+0x9` data (`0x7CCC/0x7E11`-class),
  `+0xA` IRQCFG (`0x5A56/0x6A07`-class),
  `+0xC/D/E` HWID (`0x5C69/0x5C77/0x5C83`-class).
- PIC: `0x20 x9 / 0x21 x2 (0x7360/73A1)`, `0xA0 x3 / 0xA1 x4
  (0x7383/739E/73A3)` — `0x734C` selects by IRQ (`CMP [BP+6],7; JG
  use A1 else 21`).
- PIT: `0x40/0x43` above.
- VGA: `0x3C0 x2, 0x3C2 x2, 0x3C4 x244, 0x3C5 x41, 0x3C7 x13, 0x3C8
  x9, 0x3C9 x40, 0x3CE x44, 0x3CF x1, 0x3D4 x37, 0x3D5 x9, 0x3DA x3`
  — e.g. file `0x11033/11036/1107F OUT DX,AX` (seq `3C4`),
  `0x7DF5 OUT 0x3C7 / 0x7DFC IN 0x3C9 x3` (palette DAC read),
  `0x7E3D OUT 0x3C8` + `0x3C9` writes.
- Kbd: `0x60/0x64` above. No LPT/COM (`3BC/378/3F8` zero; `COM` hits
  are only the `vfx1.com` string + `_compute_com_info` symbol).

## 6. Routine map (file/vaddr, one line each)

| File / vaddr | Purpose |
|---|---|
| `0x1800/0x0000` entry0 | Borland startup: `B4 30 CD21`, PSP `[02h]/[2Ch]` env seg, heap/`4A`, main dispatch |
| `0x183BF/0x16BF` | Print-`$` (`LDS DX,[BP+6]; B4 09 CD21`) — help/report |
| `0x1A88…` | Borland lib (mem/file/printf scaffolding, `INT 21h 40/3F/3E/3D`) |
| `0x52A7/0x3AA7, 0x52DE, 0x5305` | Mouse INT33 wrappers |
| `0x5A13/0x4213` | `VIPGetVector` (`B8 0035 CD21` get vector) |
| `0x5A2D/0x422D` | `VIPIRQSetting` (`DX+BX; +0xF 00; +0xA bits`, `CLI/POPF`) |
| `0x5A61+/0x4261` | `GetPort/VIP_Test_Port` (`DX+[BP+6]; +0xA; IN`, `B4 00` tester) |
| `0x5Cxx` | HWID block (`+0xC/D/E IN`, `+0xA/+0xF OUT`) |
| `0x6918/0x5118` | `VIPAbus` byte out (`+[8] OUT`) |
| `0x6931+/0x5131` | Status set (`+[8] IN; OR 49h AND 7Fh OUT`) |
| `0x6A63/0x5263` | `IntOFF` (`INC [18h]`) |
| `0x6A73` | `IntON` (`DEC`) |
| `0x6A83/0x5283` | Delay/poll (`XOR BX,BX; IN [10C0]; CMP BX,0DAC; JL` ≈3500x `+0x0`) |
| `0x6A9F+/0x529F` | `VIPAbusSETInit/Control` (`lcall 050B:0001 + 067E:0083`, `OR SI,SI; JE`) |
| `0x734C/0x5B4C` | PIC helper (`0x21/0xA1` mask merge with `[10AD/AE]`) |
| `0x77DF/0x5FDF` | **REPORTING sweep sender**: `MOV SI,2; CALL 6A83; PUSH SI,DI; CALL 6FA5; …imul AX,SI,7Fh +0xA4E; CALL 6A32; OR AX,AX; …MOV [5C],SI / [5A],SI; ADD SI,2; CMP SI,0Ch` — candidates 2,4,6,8,10 (+30000-class `+0x0` polls in callees; the `addr 50 83 C1 hi lo` shape) |
| `0x7C94/0x6494` | **`Device_Test`/drain**: `CALL 7295; [10BC]=AX; CLI; lcall …; DX=SI; +9 IN; +8 IN; TEST AX,20h; JNE drain; lcall 041B:0047 send` — the `RX 0x84 / or ax,ax / jne retry` flush that exits only on dry-0 |
| `0x7D3D/0x653D` | Sequencer (`PUSH [BP+8]; CALL 734C(PIC); lcall 041B:0047; AX=[10BA]&10h>>4`) |
| `0x7DA3/0x65A3` | `VIPSet` (`DX=[10C0] OUT 06; DX+1 IN→BL; bit1/2 tweak OUT`) |
| `0x7DDA/0x65DA` | Palette/sequencer (`3C7 OUT idx; 3C9 INx3; SI+9/+1 OUT 055h-added; 3C8/3C9 mirror; CALL 7DA3; SI+8 OUT 08h`) |
| `0x7ED9/0x66D9` | `VIPTester` → 1/2/3 (`CALL 7C94` port?, `CALL 77DF` sweep?, `CALL 6A63/6A73` wrap, `CMP ES:[A9],0`) |
| `0x7Fxx–0x80xx` | `Accept/Cancel/goodPort/badPort/goodIRQ/badIRQ` UI verdict setters |
| `0x110xx` | VGA BIOS (`INT 10h`) |
| `0x179xx` | Keyboard (`INT 16h`) |
| `0x17Fxx` | EMS (`INT 67h`) |
| `0x183xx` | DOS file (`INT 21h 3Dh/3Eh/3Fh/40h`), `ModifyAutoexec/ModifyBat` + `c:/d:\autoexec.bat` |
| `0x2BDE6` (data) | Packet templates fuelling sweep/IDREQ sends (not code) |

## 7. Setup / test flow

CLI (`1–5/l#/hH?`) → PSP env-scan for `VIPPORT` → baseport probe
`260/280/2A0/2C0` (table `0x59AE`, `CALL 7C94`-class echo `+0x0`,
`+0x8` status-bit-`20h` drain) → HWID `+0xC/D/E` + CFGSTATUS `+0xF` →
IRQ conflict (`CheckBlasterIRQ/CheckUltrasndIRQ/CheckMouseIRQ`
env-IRQ vs candidate `5/7/10/11/12/15`, PIC `21/A1` mask test
`VIPTestIRQ`) → REPORTING sweep `77DF` (`SI` 2,4,6,8,10,
`50 83 C1`-template send, `+0x0` 1000s-poll of ISR flags
`[5A]/[5C]`, 1.3 s-class cadence, no bus reads between sends) →
directed `02 50 81 F1`-IDREQ discovery (`VFX1 is on / PUCK is on
address: $` vs `OFFLINE`; the `[0xA2E+idx]` found-flag poll at
`070c:0145`-class sites, dispatcher-gated) → `TEST RESULTS REPORT`
(`GOOD/BAD`, `BASEPORT/INTERRUPT/VFX1INIT`, `PASSED/FAILED/
CONFLICT…JUMPERS`) → `DO YOU WANT THE AUTOEXEC.BAT MODIFIED` →
`@SET VIPPORT=<base> I<irq>` (`MODIFICATION WAS
SUCCESSFUL/UNSUCCESSFUL`, `SAVED AS autoexec.01`) → F1–F4/`DETECT` UI
loop (`INT 10h` draw, `INT 16h` F-keys, `INT 33h` mouse,
`PLEASE WAIT / TEST VFX1 / TEST PUCK MENU`, `SPACE CENTER`,
`TOP/MIDDLE/BOTTOM` views, `BEYOND TRACKING RANGE`, `VFX1/CYBER PUCK
NOT ONLINE`).

## 8. Verdict / error table

| String | Meaning |
|---|---|
| `TEST PASSED` / `TEST FAILED` | Per-stage verdict (`BASEPORT/INTERRUPT/VFX1INIT`) |
| `CONFLICT … WITH ANOTHER … MAYBE IN` | IRQ conflict (composed from tokens + env-derived IRQ) |
| `DETECT` | Auto-probe button/verb |
| `PLEASE WAIT` | Bus sweep/discovery running (sweep + directed IDREQ + drain) |
| `- TEST VFX1 MENU -` / `- TEST PUCK MENU -` | Interactive menus reached (`SPACE CENTER` + views + `ESC EXIT`; puck `TOP/MIDDLE/BOTTOM` + graphic) |
| `VFX1/CYBER PUCK NOT ONLINE` | Directed discovery got no usable reply for that device |
| `BEYOND TRACKING RANGE` | Tracking validation reject |
| `ERROR READING VFX1 DATA` / `HEADSET CORRUPTED` / `ERROR DOWNLOADING VFX1 DATA` | Download/validate failures (`vfx1.3d/cybrpuck.3d` + `DownLoadVfx1`) |
| `ERROR:001 - COULD NOT OPEN AUTOEXEC.BAT` | `AUTOEXEC.BAT` open failed |
| `ERROR:002 - COULD NOT OPEN BACKUP FILE` | `autoexec.01` backup failed |
| `ERROR:003 - DISK FULL` | Write failed |

## 9. Emulator notes (what this binary needs)

- Answer the ISA layer exactly: echo-latch `+0x0`, `+0x8` status with
  live bit `0x20`, `+0x9` data, `+0xA` IRQCFG, `+0xC/D/E` HWID
  (`0x0D`, not `0xFF`), `+0xF` CFGSTATUS — all `DX`-indirect.
- The REPORTING sweep sends per candidate addr without bus reads
  between sends, then polls `+0x0` ~30000x for ISR-set per-device flags;
  the IRQ must actually fire on bus-RX or the flags never set.
- The drain loop (`0x7C94`) retries while nonzero and exits on dry-0:
  the bus must go momentarily dry or the test spins (`PLEASE WAIT`
  forever). A permanently-full backlog wedges it.
- Directed discovery sends `02 50 81 F1 22`-family IDREQs per addr and
  files replies per dispatcher rules; the validator at
  `070c:137f`-class sites is an unarmed direct reader — it needs
  served bytes, not just armed-transfer bytes.
- Repro recipe: `rz-bin -I/-H/-S/-e` for the MZ shell, then
  `rizin -q -c "aaa; afl; iz"` plus the Capstone pass from §6 starts;
  grep ` EC ` / ` EE ` and `CD 16/21/10/33/67` for the censuses, and
  treat naive `CD xx` hits as guilty-until-proven (`83 CD` etc.).
  Overlay `0x3C6F0/0x54F6` is data — do not sweep it as code.

## 10. Claim map

| # | Claim | Evidence |
|---|---|---|
| 1 | Borland MZ, `0x41BE6` bytes, header `0x1800`, 1421 relocs, entry `0000:0000` | `rz-bin -I` + raw MZ fields + reloc table at file `0x3E` |
| 2 | 6 source modules incl. `setvip/vipabus/vipio/newvipio/support` | Symbol strings file `0x41AFF–0x41BC6` |
| 3 | Base candidates `60 02 80 02 A0 02 C0 02`, F1–F4/F5–F10 buttons | Bytes file `0x59AE`; strings `0x3256F–0x325AE` |
| 4 | Sweep `77DF` over addrs 2,4,6,8,10 with `02 50 83 C1` template | Disasm `MOV SI,2 … CMP SI,0Ch`; templates `0x2BDE6` |
| 5 | Drain `7C94` exits only on dry-0 (`TEST AX,20h; JNE`) | Disasm `+9/+8 IN` loop |
| 6 | Conflicts from `ULTRASND/BLASTER/VRMOUSE` env, not hardcoded IRQs | Env strings + `_Check*IRQ` symbols; no SB16/GUS literals |
| 7 | Overlay `0x54F6` is data (zero `in/out/int`) | Capstone-16 sweep negative |
| 8 | All bundles = 1.20a program, 1-byte label each (see §11) | Pairwise full-file diffs: single `0x33/0x35/0x36` at `0x2F993` |

## 11. Version delta: 2.13-bundle vs 2.16-bundle setup (`D:\Emu\DOS\VFX1` vs `D:\Emu\DOS\C\VRDRV\VFX1`)

Copies inventoried (SHA256): 2.13-bundle `269286 B, 1997-01-20,
C1A2BC23…51A68D4C23` — byte-identical in `D:\Emu\DOS\VFX1`,
`D:\Emu\DOS\Y-utils\VFX1`, `D:\Emu\DOS\C\VRDRV\vfx1sdk\VFX1`;
2.15-bundle `269286 B, 1997-10-24, F31E0A32…F64C074` in
`D:\Emu\DOS\C\VFX215` (from `vfx1_215.exe`); 2.16-bundle `269286 B,
1998-10-02, F61E1F7A…56D1EF1E45` — byte-identical in
`D:\Emu\DOS\C\VRDRV\VFX1` and `D:\Emu\DOS\C\VFX216`. Same size all
three; each pair differs by **exactly 1 byte** at file `0x2F993`:
`0x33` (2.13) vs `0x35` (2.15) vs `0x36` (2.16), inside
`INSTALL VERSION 2.1x` (menu/report block, same `GERMAN` neighbour
token all sides). The `VFX215` vs `VFX216` trees are otherwise fully
identical (all other 40 files hash-match).

The setup program's self-version ` Version 1.20a` is identical at the
same file offset (179475) in all three copies, and all code
disassembles identically — so every bundle's `SETVFX1.EXE` is the same
1.20a program build, relabeled per bundle (`2.13`/`2.15`/`2.16`).
Every §6 routine/file offset in this doc applies to all copies
unchanged (no shift, unlike the driver in `vfx1_drv.md` §11). Bundle
composition: 2.13 bundle = setup `2.13` + driver **2.13**; 2.15 bundle
= setup `2.15` + driver **2.14**; 2.16 bundle (`VRDRV\VFX1` and
`VFX216`) = setup `2.16` + driver **2.14** — i.e. mixed bundles, never
a code fork. The `vfx1_215.exe` follow-up is closed (extracted tree
surveyed: driver 2.14, nothing newer).

## 12. Runtime disassembly: TEST VFX1 / ABUS receive path (load-address independent)

Added 2026-10-18. The static file view (§6) is only correct at the load
linear address; at runtime every code segment shifts with the DOS load
segment (`linear = seg*16 + ip`, so a segment's bytes sit at file
`linear − load_linear + 0x1800`). Because the load segment varies per boot
(observed `0x0611` vs `0x0612` across two sessions), **disassemble from a
guest-RAM dump, not the file**. Repro:

- Temporarily dump guest RAM once on the first VIP access
  (`MEM_BlockRead(0, buf, 0x100000)`) in `vfx1_mon_trace`.
- `rizin -a x86 -b 16 -F raw -e scr.color=false -q -c "e io.va=false"
  -c "s <linear>" -c "pd N" guestmem.bin` (use `-F raw`; the COFF/BIOs
  auto-detect corrupts the view).
- Drive the menu directly: `setvfx1 1` = SETUP VIPPORT (incl. the
  `VIP CARD` detect), `setvfx1 3` = TEST VFX1, `setvfx1 4` = TEST
  CYBERPUCK, `setvfx1 2` = SETUP FOCUS, `setvfx1 5` = CALIBRATOR.
  `setvfx1 3` reproduces the TEST-menu hang headlessly, no keys needed.

`Test_Vfx1` receive loop (runtime seg `070B`; file `0x7C94`-region). SI =
VIP base:

```
83F4  push bp; mov bp,sp; push si,di,ds
83FA  mov ax,0x2C51; mov ds,ax          ; Borland per-func DS reload
83FF  mov si,[bp+6]                     ; SI = port base
8402  mov di,[bp+8]
8405  push di; push cs; call 0x79F5     ; check handle -> [0x10BC]
840E  cmp word [0x10BC],0; jz fail      ; not online -> return 1
8417  call far 0611:00B1                ; = IN +0x0A (IRQCFG)  -> [0x10BA]
8421  call far 0701:0081                ; = OUT +0x8 (read,|0x49,&0x7F) = 0x7F
8428  jmp 8430
842A  mov dx,si; add dx,9;  in al,dx    ; DRAIN: read +0x9 (discard)
8430  mov dx,si; add dx,8;  in al,dx    ;        read +0x8
8436  mov ah,0
8438  test ax,0x20                      ; bit5 = "byte available"
843B  jnz 842A                          ; loop while bit5 SET  <-- WEDGE POINT
843D  call far 0611:0063                ; = INT21/35 getvect  -> [0x9FE]:[0x9FC]
844E  call far 0611:0047                ; = INT21/25 setvect(070B:12D6 @ int [0x10BC])
8460  call far 0611:007D                ; = OUT +0xF 0; OUT +0xA irq|0x10
846C  call 0x7A4B
8472  call 0x71E3; push 0x84; push ds; push 0x3B6; push si
847E  call far 0874:0004                ; read 0x84 (132) bytes -> AX = count
8486  or ax,ax; jnz 8472                ; retry while count != 0
848E  popf; xor ax,ax; ...; retf
```

ABUS block read `read_abus(port, buf, count)` (runtime seg `0874`, file
`0x8003`-region; call `0874:0004` with args `(base, buf, 0x84)`):

```
8744  enter 2,0; push si,di,ds; mov ds,0x2D5D
8750  di = [bp+6]+8                     ; ABUS_CONTROL
8758  [bp-2] = [bp+6]+9                 ; ABUS_DATA
8763  cl = IN [di]                      ; read +0x8
876C  test ax,0x04; jnz 87BA            ; bit2 (complete) set -> return 0
8771  OUT [di] = (cl & 0xB7) | 0x01     ; ARM: set bit0, clear bit3/bit6
877E  byte = IN [[bp-2]]                ; read +0x9 data
8787  buf[si++] = byte
8796  cl = IN [di]                      ; read +0x8
879F  test ax,0x20; jnz 877E            ; while bit5 -> read next byte
87A4  test ax,0x04; jz  8796            ; while bit2 clear -> re-poll
87AD  OUT [di] = cl | 0x48              ; completion shape
87B4  mov ax,si; ret                    ; return bytes read
```

Semantics (both routines agree): `+0x8` **bit5 = "byte available"**
(read `+0x9` while set); **bit2 = "transfer complete"**; a routine arms by
writing `(cl & 0xB7) | 0x01` and finishes by writing `cl | 0x48`. Reading
with no transfer outstanding returns 0.

Root cause of the TEST VFX1 "not moving" (and the same reason the
`setvfx1 3` spin is now reproducible headlessly): with `reporting == false`
and no live test-key motion, the emulator's idle `+0x8` still reports bit5
SET (the `legacy`/`resp_ready` path), so the pre-arm drain at `8438` never
goes dry; and because the reader is unarmed and no RX backlog is queued,
`read_abus` would find nothing to serve even after the drain. The card must
present a tracking-report block on the ABUS when the TEST menu asks
(bit5 pulsing out the 132-byte block, bit2 set on completion), and idle
`+0x8` bit5 must read dry when no bytes are outstanding.
