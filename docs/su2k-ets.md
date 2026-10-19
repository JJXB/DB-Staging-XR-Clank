# ETS.EXE — Disassembly Atlas

`D:\Emu\DOS\C\V2k\VR 2000 Series Disc Image (Simon's)\ETS\ETS.EXE`
(1350529 B). Companion machine-readable file: `su2k-ets-atlas.txt`
(same directory as this doc) — full function table, top callees,
IN/OUT/INT sites, string table. This doc is the reader's guide:
what was decoded live, what the atlas holds, and grep recipes so
nobody re-decompiles from scratch.

Method: Capstone 5.0.7 linear sweep (`skipdata`) of the code object
(143130 insns), Watcom-prologue + call-target function split (2553
functions), raw-ASCII string census (26997 runs ≥5), LE object map
(`rz-bin -S/-e`). No full `aaa` analysis (too slow at this size —
aborted at 180 s in earlier attempts); linear sweep desyncs inside
data/RELOC regions, so single-instruction reads there need a true
anchor (verified byte patterns below). Rizin `rizin -q` remains the
tool for targeted per-function reads.

## 1. Layout + address mapping (verified, not assumed)

- LE 32-bit, Watcom 10.0a + Rational DOS/4GW, 80386. Objects:
  `obj1` code `paddr 0x19C00` size `0x72000` (`vaddr 0x10000`);
  `obj2` `0x8BC00/0x1000` (`0x90000`); `obj3` data `0x8CC00/0x36A54`
  (`0xA0000`, vsize `0xD4950` with BSS). Entry `vaddr 0x48528`
  (`paddr 0x52128`) = `jmp` stub over data (same shape as DN2/ZONE).
- **Runtime = file + `0x15C400` inside obj1**, proven at 6+ independent
  spots (TRK loop `0x1CB142`, helpers `0x1CCE46/4E/55`,
  data reader `0x1D913F`, caller `0x1A655E`, string bytes
  `0x1CB50C`), i.e. obj1 loads at linear `0x170000`. It holds code
  AND const data intermixed (string literals beside functions).
- obj3 (writable data) runtime base unresolved — live dumps never
  needed it (all observed strings were obj1 copies). Data xrefs that
  resolve below `0x170000` are obj3-relative and need its base first.
- TRK open strings exist in **two copies** inside obj1 (file `0x5E7xx`
  and `0x6F1xx`, both fully enumerated in §4); the live copy is the
  `0x6F1xx` one (runtime `0x1CB5xx`). Static byte-search for xrefs
  fails (LE fixups patch references at load) — xrefs below come from
  live backtraces, not from scanning.

## 2. Function inventory (from `su2k-ets-atlas.txt`)

- 2553 functions by prologue scan (`push ebx/ecx/esi/edi/ebp;
  mov ebp,esp` family) + `E8` call-target split + entry.
- Top shared helpers by call count (runtime; file = rt − `0x15C400`):
  `0x19915E` ×1036 (allocator — every `push imm; call` malloc site),
  `0x17924E` ×479, `0x19AFE0` ×431, `0x1CCE4E` ×284 (data-OUT byte
  helper — all `W`/`S`/command writes flow here),
  `0x1B2F60` ×281, `0x1780EE` ×278, `0x1CCE46` ×140 (status-IN byte
  helper — all status polls), `0x1AC28D` ×232, `0x1D913F` word-IN
  helper (data reads; lower count because vtable-called).
- Port IO is ~entirely DX-variable (203 sites through the helpers;
  fixed-port `in/out` immediates: none of consequence) — ports come
  from `config.vpc` at runtime, so static port scans are blind by
  design. Real INT usage: `0x21` ×85 (DOS), `0x10` ×69 (video),
  `0x31` ×18, `0x2F` ×8 (MSCDEX), `0x33` ×1 (mouse). The one-hit
  `int 0x00–0xFF` spread is a 256-entry dispatch *table* (data),
  not 256 call sites.

## 3. Decoded flows (runtime addresses; file = rt − `0x15C400`)

- **TRK send/recv core**: byte-sender `~0x1CB065` (sends `buf[i]`
  via `0x1CCE4E`, `delay(10)` when `!(status & 2)`), word-reader
  `~0x1CB125` (`counter++; s=status(port+1); exit if (!(s & 1));
  exit if (counter >= limit); buf[counter]=word(port)`), status
  helper `0x1CCE46` (`in al,dx`), OUT helper `0x1CCE4E`,
  word-IN helper `0x1D913F` (`in ax,dx`, port in EAX), IO base
  from global `0x295810`, all calls via vtable (no static xrefs).
- **TRK open** (`Trying to open card…`, strings file `0x6F10C+`):
  status read → `W` → sign-on drain → substring check →
  `S` → 28-word status record → firmware/sensor validation →
  `Card %ld opened successfully` / `could not be opened`.
  S layout still open (marker probe: count passes, content gated).
- **Retry driver** `~0x1CA4E7`: opens with arg 1 then arg 100,
  `test eax,eax; jnz` — the 4× `W+S` cycles in the logs.
- **Packet pump** `~0x1D9080`: word-packet dispatcher, `case 0x0B`
  via jump table `0x1D8FB0`, indirect `call [esi+4]` service
  routines.
- **FORMAT init caller** `0x1A655E` (file `0x4A15E`): computes
  `(segment, offset, count)` with `count = (x<<10 − [ebx+0x36]*0x600)`,
  far-memsets `0x52` via Watcom helper `0x1CCE5C`
  (`push es; mov es,edx; rep stos; pop edi; pop es`), then
  `mov ax,[ebx]; add eax,0x10; call` + `test [eax+0x4E],0x50`.
  Its static siblings: memset calls at file `0x4A10F`/`0x4A159`.
- **Crash site**: `pop es` at `0x1CCE71` with entry-ES `0x168`
  (dead) → `#NP`; GDT index `0x2D` found smashed with `0x52`
  (base `0x52525252`, P=0) → an earlier wild `0x52` fill is the
  silent killer, the `pop` just surfaces it. Ancestor frames:
  `0x1A655E` ← `0x1AC27A` (byte-queue put) ← `0x1AC2FF`
  (`push es; mov ax,ds; mov es,eax` + `repne scasb` strlen —
  SETS ES valid, so likely stale frame, not live ancestor).
  Open question: which invocation ran wild (count operands from
  card-struct fields — see §5).

## 4. String blocks of interest (file offsets)

- TRK open: `0x6F10C` (`Trying…`), `0x6F149` (`Attempting…status`),
  `0x6F173/1BB` (`%ld words received[, 28 expected]`),
  `0x6F187/19E/1A2` (`System status record:`/`%s`/`%f`),
  `0x6F1A5` (`Firmware version %ld`), `0x6F1DC+` (`InsideTRAK`
  checks ×2), `0x6F24E/26C` (verdicts), `0x6F298+` (sensor
  commands `l1,1/l2,1/y0/y1/h,/A/R/s`, `Send %ld bytes`),
  `0x6F24E`-dup at `0x5E800` block.
- SND error fmts near runtime `0x1AC25A` (file `0x4FE5A`):
  `Card 1`, `%s`, `SND Error : (%d) %s`.
- Flag files (0 B, enable logging): `ETSDEBUG`, `TRKLOG`;
  outputs `c:\trk1.dat`, `snd.log`, `net.log`, `vpclog`.

## 5. Open questions with addresses attached

1. S-record layout: which words hold firmware version / sensor
   bitmap / checksum — read the validator between the S-drain and
   the verdict `printf` (fmts near runtime `0x1CB758`-class data;
   start disassembly at file `0x6ECCC` region already mapped).
2. Wild-fill count: `[ebx+0x36]` word and `EDX<<10` inputs at file
   `0x4A130+` (runtime `0x1A6530+`) — identify the card struct and
   which emulator-fed bytes reach it.
3. ES origin: 64-slot stack walk + `mov es,` sites in init path
   (only `0x1AC2FF`-class setters found so far).
4. obj3 runtime base (for the handful of sub-`0x170000` data refs).

## 6. Grep recipes (no re-decompiling)

- Find a function's code: `Select-String ets-atlas.txt -Pattern
  "^0x001CB142"` → `file = rt − 0x15C400` → `rizin -q -c
  "s <file>; pd 60" ETS.EXE`.
- Find who calls helper H: grep `H` in TOP CALLEES → e.g.
  addresses per caller line.
- Find a string's neighborhood: string table gives file offsets;
  disassemble ±256 B around them for the owning printer.
- Verify an address: recompute `file + 0x15C400 == runtime` against
  any logged bytes before trusting a sweep.

## 7. Claim → source map

- Layout/objects/entry → `rz-bin -I/-S/-e` on `ETS\ETS.EXE`.
- 143130 insns / 2553 functions / 1625 IO sites / string table →
  `su2k-ets-atlas.txt` (Capstone sweep, this session).
- Runtime↔file delta → 6+ live-log byte matches (§1).
- All flow addresses → stuck-dump + abort forensics in
  `dosbox-xr.log` + targeted rizin reads (see `PROGRESS.md`
  #78–#97).
- Strings → raw-ASCII census + `trkctx.py` neighborhood dump.
