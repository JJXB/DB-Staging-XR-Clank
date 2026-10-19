# Descent 2 — VFX1 Alternate 3D Mode Driver Infodump

Source binary: `D:\Emu\DOS\C\desc2vfx\D2VFX1.EXE` (**5,442 bytes = `0x1542`**, modified 1996-01-11), a 16-bit MZ DOS EXE (arch x86, bits 16). Related: `VFX1D2.EXE` (19,847 bytes, same dir, dated 2026-10-15). Scope: FORTE's Descent 2 VFX1 HEADGEAR Alternate 3D mode driver — a resident TSR that hooks video/interrupt vectors to feed VFX1 tracking into Descent 2. Nothing below comes from emulator source — only strings + rizin disassembly of the DOS binary.

## 1. The 30-second version

`D2VFX1.EXE` is a tiny TSR (5.4 KB) that must be copied into the Descent 2 directory and run via `VRD2.BAT` instead of the normal Descent 2 startup. It requires Descent 2 SETUP's VR Devices set to **NONE** and VFX1 utilities v2.0. It installs interrupt hooks (INT 0x35–0x39 VFX1 API vectors, INT 0x10 video, INT 0x1A time-of-day), resizes its memory block, then terminates-and-stays-resident. Descent 2 then reads head-tracking data through the hooked vectors.

## 2. MZ header / layout

| Field | Value |
|---|---|
| Signature | `MZ` (file `0x00`) |
| Last-page size | `0x0142` = 322 bytes |
| Pages in file | `0x000B` = 11 |
| Relocation entries | `0x0020` = 32 |
| Header size | `0x000D` paragraphs = 208 bytes (program image at file `0x0D0`) |
| Min/max alloc | `0xFFFF` / `0x0139` paragraphs |
| Initial SS:SP | `0x0080`:`0x0000` |
| Initial CS:IP | `0x0000`:`0x0000` |
| Checksum | `0x0000` |
| Reloc table offset | `0x003E` |

Program image begins at file offset `0x0D0`; first ~0x130 bytes of image are zero-fill; real code starts at `0x200`.

## 3. Disassembly — entry & TSR install (`0x200`–`0x3A7`)

```
0x200: mov dx, 0x101
0x203: mov word cs:[0x235], dx      ; save DS/CS segment at [0x235]
0x208: mov ah, 0x30                 ; DOS get version
0x20A: int 0x21
0x20C: mov bp, word [2]             ; PSP segment (word at 0x0002)
0x210: mov bx, word [0x2C]          ; environment segment
0x214: mov ds, dx
0x216: mov word [0x92], ax          ; save DOS version
0x219: mov word [0x90], es
0x21D: mov word [0x8C], bx
0x221: mov word [0xA8], bp
0x225: call 0x365                   ; hook INT 0x35–0x39 (VFX1 API vectors)
0x228: les di, dword [0x8A]         ; recover old INT 0x35 vector?
0x22C–0x243: strlen scan of ES:DI buffer (repne scasb, CX=0x7FFF)
0x247–0x252: CX = ~CX; [0x8A] = CX; BX = (CX<<1)+8 & 0xFFF8; [0x8E] = BX
0x256–0x27E: compute residency size: min(di-0x200, 0x200) + [0x294]=0x8B07, paragraph-round
0x280–0x29C: if [0x29A]==0 && [0x294]==0 jump 0x28E; else DI=0x1000
0x29C–0x2A0: [0xA0] = DI+DX; [0xA4] = BX  (new ES and memory size)
0x2A8–0x2BD: INT 21h AH=4Ah (set block/resize memory) with BX=new size
0x2B7–0x2BA: cli; mov ss, dx; mov sp, di; sti   (relocate stack)
0x2BF–0x2C4: es = word cs:[0x235]; rep stosb 0x374−0x334 bytes at ES:0x334  (zero BSS)
0x2CF–0x2D7: INT 1Ah AH=00 (time of day) → [0x96]=DX, [0x98]=CX (timer seed)
0x2DB–0x2E2: bp=0; ds=0x101; si=0x322; di=0x334; call 0x3D5   (memcpy 0x2F bytes from 0x322 to 0x334, XOR-scan for 0xFF)
0x2EB–0x30B: restore ds; set flags bytes; push old INT vectors [0x88]/[0x86]/[0x84]; call 0x439 (install INT 0x36/0x37/0x38 hooks?)
0x30C: call 0x5C6
0x30F–0x31D: ds=0x101; si=di=0x334; call 0x3D5 (second memcpy)
0x322: call word [0x126]; call word [0x128]; call word [0x12A]  (chain to 3 hooked handlers)
0x333: call 0x3A8
0x336–0x348: checksum: AX=0; sum bytes [SI] for CX=0x2F (47) bytes; if AX==0xD5C jump 0x353 else print error via 0x418 (INT 21h AH=40 write to stdout) with CX=0x19
0x353–0x35A: bp=sp; AH=0x4C; AL=[bp+2] (exit code); INT 21h terminate — **but this is the TSR install path**
0x35C–0x362: CX=0x0E; DX=0x48 ('H'); jmp 0x426   (TSR via AH=0x31 at 0x426)

Function 0x365–0x3A7 (install INT 0x35–0x39 hooks):
- INT 21h AH=0x35 AL=00..04 → get vectors into BX:ES at [0x74]/[0x76], [0x78]/[0x7A], [0x7C]/[0x7E], [0x80]/[0x82]
- INT 21h AH=0x25 AL=00..04 → set vectors to CS:0x15C (INT 0x35), CS:0x15C (0x36), CS:0x15C (0x37), CS:0x15C (0x38) — all four VFX1 API vectors point at one handler
- RETF

Function 0x3A8–0x3D4 (restore INT 0x35–0x39):
- For AL=00..04: INT 21h AH=0x25 with DS:DX = saved vectors at [0x74]/[0x76] etc. (uninstall)

Function 0x3D5–0x417 (memcpy with 0xFF sentinel):
- AH=0xFF; scan [BX] vs [DI], copying 6 bytes at a time until BX==DI or byte==0xFF or byte>0xFF; used to copy the resident body to its final location

Function 0x418–0x41F (write to stdout):
- AH=0x40, BX=0x02; INT 21h; RET

## 4. Resident body (`0x420`–`0x4C2+`)

Entry at `0x426` (TSR install):
- `0x420`: CX=0x1E; DX=0x56 ('V'); `0x426`: ds=cs:[0x235]; call 0x418 (print 'V'...?)
- `0x42E`: AX=3; push; call 0x32E (probably INT 0x31/0x33 install)
- `0x439`–`0x4C2`: INT 0x21 AH=0xAA (install check), AH=0xB3?? (0xAA/0xB3 are VFX1/ACI calls), INT 0x1Ah time, stack frame setup with 0x5A bytes locals, repeated calls to 0xD54 and 0xCBA (ACI packet send/receive), INT 21h AH=0x103, character echo loop (0x468–0x473: 9 chars), more ACI calls at 0x47D/0x48B/0x4A3, then INT 21h AH=0x114/0x114 (ACI vector calls).

Function at `0x4C3`–`0x4DF+`:
- `0x4C6`: if word [0x124] != 0x20 jump 0x4D2; else AX=1, jump 0x4E5
- `0x4D2`: BX=[0x124]<<1; AX=[bp+4]; [BX+0x334]=AX; inc word [0x124]   (ring buffer of 16-bit values at 0x334, indexed by [0x124])

## 5. INT census (true code)

| INT | AH/AL | Purpose | File off |
|---|---|---|---|
| 21h | 30h | DOS version | `0x208` |
| 21h | 35h | Get vector (0x35–0x39) | `0x369`–`0x390` |
| 21h | 25h | Set vector (0x35–0x39 install) | `0x376`–`0x3A4` |
| 21h | 25h | Restore vector (0x35–0x39) | `0x3B0`–`0x3D1` |
| 21h | 4Ah | Set block (resize memory) | `0x2B2` |
| 1Ah | 00h | Time of day (timer seed) | `0x2D1` |
| 21h | 4Ch | Terminate (exit code from [bp+2]) | `0x357` |
| 21h | 40h | Write to stdout (error message) | `0x41D` |
| 21h | 103h / 114h | ACI / VFX1 API calls | `0x464`, `0x4B2`/`0x4B9` |
| 21h | 0xAA / 0xB3 | ACI install-check / API | `0x43F`–`0x44C`, `0x45F`–`0x467` |

No direct VIP `IN`/`OUT` immediate-port I/O — all VFX1 communication goes through the hooked INT 0x35–0x39 vectors and ACI calls.

## 6. Usage (per D2VFX1.TXT)

1. Copy `D2VFX1.EXE` into your Descent 2 directory (default `C:\GAMES\D2`).
2. Set Descent 2 SETUP VR Devices to **NONE**.
3. Requires VFX1 HEADGEAR utilities v2.0 installed.
4. Run `VRD2.BAT` instead of Descent 2's normal startup.
5. In-game: 3D video displays on the VFX1 HEADGEAR.

## 7. Claim map

| # | Claim | Evidence |
|---|---|---|
| 1 | `D2VFX1.EXE` is FORTE Descent 2 VFX1 alternate 3D driver | Strings + `D2VFX1.TXT` |
| 2 | TSR hooks INT 0x35–0x39 (VFX1 API vectors) | Disassembly `0x365`–`0x3A7`, INT 21h AH=25h/35h with AL=0..4 |
| 3 | Hooks INT 0x10 (video snoop) via INT 21h AH=25h chain at `0x322`–`0x32A` | `call word [0x126/0x128/0x12a]` after saving old vectors |
| 4 | Resizes memory via INT 21h AH=4Ah then TSR-exits | Disassembly `0x2AF`–`0x2B2`, `0x35C`–`0x362` |
| 5 | Checksum 0xD5C over 47 bytes at `0x334`; error via INT 21h AH=40 | Disassembly `0x336`–`0x350` |
| 6 | Requires Descent 2 VR Devices = NONE, VFX1 utils v2.0, run via VRD2 | `D2VFX1.TXT` |
