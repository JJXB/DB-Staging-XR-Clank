# PIX 1000 (SU-2000) PLD reverse engineering

Source ROMs: `D:\Emu\DOS\C\V2k\SU2000 ROMS\PIX1\WI01P*.bin` (22 parts).
Decoded equations: `D:\Emu\DOS\C\V2k\SU2000 ROMS\PIX1_EQNS\*.eqn` (UTF-8).

Tooling: MAME `jedutil` rebuilt from source (`src/tools/jedutil.cpp` + lib/util) with MSVC;
raw 0/1 fuse maps packed big-endian (`pack.py`) then `jedutil -view`.

## Part inventory

| Part | Type | I/O | Role (hypothesis) |
|---|---|---|---|
| U6, U26 | PALCE16V8 | 5-bit 2:1 mux (sel i1) | data-path mux between two 5-bit buses |
| U44, U54 | PALCE16V8 | 4/5-bit 2:1 mux (sel i9) | data-path mux; U54 pin15 = GND |
| U50 | PALCE16V8 | tri-state buf + 2:4 dec | enable/buffer stage (i9 enables buffers, i5 enables decode) |
| U45 | PALCE16V8 | 8 registered | control/status register |
| U46, U67, U80 | PALCE16V8 | mixed reg/comb | register with async outputs |
| U53 | PALCE16V8 | mixed | control register + decodes |
| U56, U58, U75 | PALCE16V8 | registered | counters / state registers |
| U79 | PALCE16V8 | 2 reg + 1 comb | small state + decode |
| U48, U49 | GAL20V8 | 8 registered | video pipeline sequencers (5 state bits, i2-i11 inputs) |
| U70 | GAL20V8 | mixed | address decode + register |
| U72 | GAL20V8 | mixed | combinatorial decode + registered |
| U73 | GAL20V8 | combinatorial | address decode (terms on i1,i8,i9,i10,i11,i14,i23) |
| U74, U83, U92 | GAL20V8 | 8 registered | registered pipelines/state machines |

## Key observations

- J9 selects I/O base `0x280..0x3E0`; `config.vpc` uses `0x2A0/0x2A8` (proc) and
  `0x300/0x360` (proc), `0x320` (broadcast), `0x340` (video).
- ETS reads one byte from `0x300` after every broadcast block and expects `0xCF`;
  that read is served by the PIX card's combinatorial decode (U73/U50 family).
- U48/U49 are the core video sequencers: 8 registered outputs fed back as state,
  with input terms including address/control lines i2-i11 and feedback rf18-rf22.

## C++ modules

- `src/hardware/pix1000.h` — 22 decoded PAL structs (e.g. `pix1000::Pal_WI01P006_U6`),
  each with `eval(const int* in)` computing combinational + registered outputs
  from the decoded equations. Also declares `SU2000_PIX1000_AddConfig/Install/
  Uninstall/Installed`.
- `src/hardware/pix1000.cpp` — DOSBox IO device: instantiates all 22 PALs,
  installs read/write handlers at the configurable proc1/proc2 bases, and
  evaluates the PALs on every access. Currently a **placeholder netlist**:
  every PAL input pin is driven from a port-address bit (input pin iN ->
  address bit N-1 mod 16). Reads answer the observed `0xCF`; writes are
  logged. Full-accuracy PAL simulation is gated behind PCB netlist mapping.
- Wired into `src/hardware/su2000.cpp` (config section, install/uninstall,
  ISA claim map) and `src/hardware/CMakeLists.txt`.

Config keys (section `[su2000]`, master switch `su2000_enable` must be on):
- `pix1000_enable` (OnlyAtStart, off) — enable the PIX 1000 device
- `pix1000_log` (Always, on) — log port accesses with CS:IP
- `pix1_proc` (OnlyAtStart, 0x300) — channel-1 proc IO base
- `pix2_proc` (OnlyAtStart, 0x360) — channel-2 proc IO base

Note: `pix_trap` (log-only stub in su2000_traps.cpp) and `pix1000_enable`
both answer at the proc ports; enable only one at a time to avoid handler
collisions.

## Next steps

1. Map PAL pin numbers to the PIX 1000 netlist using board photos (`pix1000_1.png`)
   and the J9 table; resolve which inputs are address bits vs. control strobes.
2. Translate each PAL's equations into a C++ logic block in `src/hardware/su2000.cpp`
   (or a new `src/hardware/pix1000.cpp`) so reads/writes at the J9 bases exercise
   the real decode instead of the current `0xCF` stub.
3. Feed the U48/U49 state machines with the broadcast FIFO stream to reproduce the
   ETS boot screen and in-game rendering.
PY
## Full equations (reference)

### WI01P006.U6.eqn

```
Inputs:

1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 12, 13, 14, 17, 18, 19

Outputs:

15 (Combinatorial, No output feedback, Active low)
16 (Combinatorial, No output feedback, Active low)
17 (Combinatorial, Output feedback output, Active low)
18 (Combinatorial, Output feedback output, Active low)
19 (Combinatorial, Output feedback output, Active low)

Equations:

/o15 = /i1 & /i6 +
       i1 & /i13
o15.oe = vcc

/o16 = /i1 & /i5 +
       i1 & /i11
o16.oe = vcc

/o17 = /i1 & /i4 +
       i1 & /i9
o17.oe = vcc

/o18 = /i1 & /i3 +
       i1 & /i8
o18.oe = vcc

/o19 = /i1 & /i2 +
       i1 & /i7
o19.oe = vcc
```

### WI01P026.U26.eqn

```
Inputs:

1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 12, 13, 14, 17, 18, 19

Outputs:

15 (Combinatorial, No output feedback, Active low)
16 (Combinatorial, No output feedback, Active low)
17 (Combinatorial, Output feedback output, Active low)
18 (Combinatorial, Output feedback output, Active low)
19 (Combinatorial, Output feedback output, Active low)

Equations:

/o15 = /i1 & /i6 +
       i1 & /i13
o15.oe = vcc

/o16 = /i1 & /i5 +
       i1 & /i11
o16.oe = vcc

/o17 = /i1 & /i4 +
       i1 & /i9
o17.oe = vcc

/o18 = /i1 & /i3 +
       i1 & /i8
o18.oe = vcc

/o19 = /i1 & /i2 +
       i1 & /i7
o19.oe = vcc
```

### WI01P044.U44.eqn

```
Inputs:

1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 13, 14, 15, 16, 17, 18

Outputs:

13 (Combinatorial, Output feedback output, Active low)
16 (Combinatorial, Output feedback output, Active low)
17 (Combinatorial, Output feedback output, Active low)
18 (Combinatorial, Output feedback output, Active low)
19 (Combinatorial, No output feedback, Active low)

Equations:

/o13 = /i9 & /i14 +
       i9 & /i15
o13.oe = vcc

/o16 = /i4 & /i9 +
       /i8 & i9
o16.oe = vcc

/o17 = /i3 & /i9 +
       /i7 & i9
o17.oe = vcc

/o18 = /i2 & /i9 +
       /i6 & i9
o18.oe = vcc

/o19 = /i1 & /i9 +
       /i5 & i9
o19.oe = vcc
```

### WI01P045.U45.eqn

```
Inputs:

2, 3, 4, 5, 6, 7, 8, 9, 12, 13, 14, 15, 16, 17, 18, 19

Outputs:

12 (Registered, Output feedback registered, Active high)
13 (Registered, Output feedback registered, Active high)
14 (Registered, Output feedback registered, Active high)
15 (Registered, Output feedback registered, Active high)
16 (Registered, Output feedback registered, Active high)
17 (Registered, Output feedback registered, Active high)
18 (Registered, Output feedback registered, Active high)
19 (Registered, Output feedback registered, Active high)

Equations:

rf12 := i2 +
        i3 & /i6 & i8 & i9 +
        i3 & /i6 & i7 & i9 +
        i3 & /i5 & i8 & i9 +
        i3 & /i4 & i7 & i8 & i9
rf12.oe = OE

rf13 := i2 +
        i3 & /i6 & i8 & i9 +
        i3 & /i6 & i7 & i9 +
        i3 & /i5 & i8 & i9 +
        i3 & i4 & i7 & i8 & i9
rf13.oe = OE

rf14 := i2 +
        i3 & /i6 & i8 & i9 +
        i3 & /i6 & i7 & i9 +
        i3 & i5 & i8 & i9 +
        i3 & /i4 & i7 & i8 & i9
rf14.oe = OE

rf15 := i2 +
        i3 & /i6 & i8 & i9 +
        i3 & /i6 & i7 & i9 +
        i3 & i5 & i8 & i9 +
        i3 & i4 & i7 & i8 & i9
rf15.oe = OE

rf16 := i2 +
        i3 & i6 & i8 & i9 +
        i3 & i6 & i7 & i9 +
        i3 & /i5 & i8 & i9 +
        i3 & /i4 & i7 & i8 & i9
rf16.oe = OE

rf17 := i2 +
        i3 & i6 & i8 & i9 +
        i3 & i6 & i7 & i9 +
        i3 & /i5 & i8 & i9 +
        i3 & i4 & i7 & i8 & i9
rf17.oe = OE

rf18 := i2 +
        i3 & i6 & i8 & i9 +
        i3 & i6 & i7 & i9 +
        i3 & i5 & i8 & i9 +
        i3 & /i4 & i7 & i8 & i9
rf18.oe = OE

rf19 := i2 +
        i3 & i6 & i8 & i9 +
        i3 & i6 & i7 & i9 +
        i3 & i5 & i8 & i9 +
        i3 & i4 & i7 & i8 & i9
rf19.oe = OE
```

### WI01P046.U46.eqn

```
Inputs:

2, 3, 4, 5, 6, 7, 8, 9, 12, 13, 14, 15, 16, 17, 18, 19

Outputs:

12 (Combinatorial, Output feedback output, Active low)
13 (Combinatorial, Output feedback output, Active low)
14 (Registered, Output feedback registered, Active low)
15 (Registered, Output feedback registered, Active low)
16 (Registered, Output feedback registered, Active low)
17 (Registered, Output feedback registered, Active low)
18 (Combinatorial, Output feedback output, Active low)
19 (Combinatorial, Output feedback output, Active low)

Equations:

/o12 = /i2 & i4 & /i8 +
       /i2 & i4 & /i7 +
       /i2 & i4 & /i6
o12.oe = vcc

/o13 = /i2 & i4 & /i8 +
       /i2 & i4 & /i7 +
       /i2 & i4 & /i6
o13.oe = vcc

/rf14 := /i3
rf14.oe = OE

/rf15 := /i3
rf15.oe = OE

/rf16 := /i3
rf16.oe = OE

/rf17 := /i3
rf17.oe = OE

/o18 = i2 & i4 & /i8 +
       i2 & i4 & /i7 +
       i2 & i4 & /i6
o18.oe = vcc

/o19 = i2 & i4 & /i8 +
       i2 & i4 & /i7 +
       i2 & i4 & /i6
o19.oe = vcc
```

### WI01P048.U48.eqn

```
Inputs:

2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23

Outputs:

15 (Registered, Output feedback registered, Active low)
16 (Registered, Output feedback registered, Active low)
17 (Registered, Output feedback registered, Active low)
18 (Registered, Output feedback registered, Active low)
19 (Registered, Output feedback registered, Active high)
20 (Registered, Output feedback registered, Active low)
21 (Registered, Output feedback registered, Active low)
22 (Registered, Output feedback registered, Active low)

Equations:

/rf15 := /i6 & rf18 & rf19 & rf20 & rf21 +
         rf20 & /rf21 & /rf22 +
         rf18 & /rf19 & /rf21 & /rf22
rf15.oe = OE

/rf16 := /rf19 & rf20 & rf22 +
         rf19 & /rf20 & rf21 +
         /rf18 & /rf20 & /rf21 & rf22 +
         rf18 & rf19 & rf20 & /rf22 +
         /i6 & rf18 & rf19 & rf21
rf16.oe = OE

/rf17 := /rf19 & /rf21 & rf22 +
         rf19 & /rf20 & rf22 +
         /rf18 & /rf19 & rf20 +
         /rf18 & rf20 & rf21 +
         rf19 & rf20 & /rf21 & /rf22 +
         i6 & /i11 & rf19 & rf20 & rf21 +
         /i3 & i4 & rf18 & /rf19 & rf22 +
         /i2 & /i5 & i6 & /i7 & /i8 & /i9 & /i10 & rf19 & rf20 & rf21
rf17.oe = OE

/rf18 := /rf19 & rf20 & /rf21 & rf22 +
         rf19 & /rf20 & /rf21 +
         rf19 & /rf22 +
         /rf18 & rf19 & rf20 & rf21 +
         rf18 & /rf20 & /rf22 +
         i6 & /i11 & rf19 & rf20 & rf21 +
         rf18 & /rf19 & /rf20 & rf21 +
         /i2 & /i5 & i6 & /i7 & /i8 & /i9 & /i10 & rf19 & rf20 & rf21
rf18.oe = OE

rf19 := /rf18 & /rf20 & rf21 +
        /rf18 & rf20 & /rf21 & rf22 +
        /i3 & i4 & /rf20 & rf21 +
        /rf18 & /rf20 & /rf22 +
        rf18 & rf19
rf19.oe = OE

/rf20 := rf19 & /rf20 +
         rf18 & /rf19 & rf21 +
         /rf18 & /rf20 & /rf21 & rf22 +
         rf18 & /rf19 & /rf22
rf20.oe = OE

/rf21 := rf18 & rf19 & /rf20 +
         /i6 & rf18 & rf19 & rf21 +
         rf20 & /rf22 +
         /rf18 & /rf21 & rf22 +
         rf18 & /rf19 & /rf21
rf21.oe = OE

/rf22 := rf20 & /rf22 +
         rf18 & /rf22 +
         /i6 & rf18 & rf19 & rf20 & rf21
rf22.oe = OE
```

### WI01P049.U49.eqn

```
Inputs:

2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23

Outputs:

15 (Registered, Output feedback registered, Active low)
16 (Registered, Output feedback registered, Active low)
17 (Registered, Output feedback registered, Active low)
18 (Registered, Output feedback registered, Active low)
19 (Registered, Output feedback registered, Active low)
20 (Registered, Output feedback registered, Active low)
21 (Registered, Output feedback registered, Active high)
22 (Registered, Output feedback registered, Active low)

Equations:

/rf15 := /i8
rf15.oe = OE

/rf16 := i2 & /i5 +
         i2 & /i3 & /i6 +
         i2 & /i4
rf16.oe = OE

/rf17 := rf17 & rf18 +
         /rf17 & /rf18
rf17.oe = OE

/rf18 := rf18
rf18.oe = OE

/rf19 := i2 & i3 & i4 & /i5 & /i6
rf19.oe = OE

/rf20 := i2 & /i3 & /i5 & /i6 +
         i2 & i3 & i4 & /i5 & i6 +
         i2 & i3 & /i4 & i5 & i6
rf20.oe = OE

rf21 := rf15 & rf21 +
        /i2 & /i3 & /i4 & /i5 & /i6 & rf15
rf21.oe = OE

/rf22 := /i2 & /rf22 +
         /i3 & /rf22 +
         /i4 & /rf22 +
         i5 & /rf22 +
         i6 & /rf22 +
         /i7 & /i9 & /i10 & /i11 & /i14 & /i23
rf22.oe = OE
```

### WI01P050.U50.eqn

```
Inputs:

1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 13, 14, 15, 16, 17, 18

Outputs:

13 (Combinatorial, Output feedback output, Active low)
14 (Combinatorial, Output feedback output, Active low)
15 (Combinatorial, Output feedback output, Active low)
16 (Combinatorial, Output feedback output, Active low)
17 (Combinatorial, Output feedback output, Active low)
18 (Combinatorial, Output feedback output, Active low)
19 (Combinatorial, No output feedback, Active low)

Equations:

/o13 = /i8
o13.oe = /i9

/o14 = /i7
o14.oe = /i9

/o15 = /i6
o15.oe = /i9

/o16 = /i2 & /i3 & /i5
o16.oe = vcc

/o17 = i2 & /i3 & /i5
o17.oe = vcc

/o18 = /i2 & i3 & /i5
o18.oe = vcc

/o19 = i2 & i3 & /i5
o19.oe = vcc
```

### WI01P053.U53.eqn

```
Inputs:

2, 3, 4, 5, 6, 7, 8, 9, 12, 13, 14, 15, 16, 17, 18, 19

Outputs:

12 (Registered, Output feedback registered, Active low)
13 (Combinatorial, Output feedback output, Active low)
14 (Registered, Output feedback registered, Active low)
15 (Registered, Output feedback registered, Active high)
16 (Registered, Output feedback registered, Active high)
17 (Registered, Output feedback registered, Active high)
18 (Combinatorial, Output feedback output, Active high)
19 (Combinatorial, Output feedback output, Active low)

Equations:

/rf12 := /i3 & rf15 +
         /i3 & rf16
rf12.oe = OE

/o13 = /i3 & rf15
o13.oe = vcc

/rf14 := rf15 & rf16 & rf17
rf14.oe = OE

rf15 := rf15 & rf16 +
        /i3 & rf15 & /rf17 +
        /i3 & /i5 & rf15 +
        /i3 & /i4 & rf15 +
        /i3 & /i4 & rf16 & /rf17
rf15.oe = OE

rf16 := /rf15 & rf16 & rf17 +
        rf15 & rf16 & /rf17 +
        /i7 & /rf15 & rf17 +
        /i3 & rf16 & /rf17
rf16.oe = OE

rf17 := rf15 & rf16 +
        /rf15 & /rf16 & rf17 +
        /i3 & /rf15 & /rf16 +
        /i7 & rf16 & rf17 +
        /i3 & /i4 & i5 & /rf16 & rf17
rf17.oe = OE

o18 = /i2 & /i5 +
      i2 & rf15 & /rf16 & o18 +
      rf15 & /rf16 & rf17 & o18
o18.oe = vcc

/o19 = /i8 +
       rf15 & /rf16 & /rf17
o19.oe = vcc
```

### WI01P054.U54.eqn

```
Inputs:

1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 12, 13, 14, 17, 18, 19

Outputs:

15 (Combinatorial, No output feedback, Active high)
16 (Combinatorial, No output feedback, Active low)
17 (Combinatorial, Output feedback output, Active low)
18 (Combinatorial, Output feedback output, Active low)
19 (Combinatorial, Output feedback output, Active low)

Equations:

o15 = 
o15.oe = vcc

/o16 = /i4 & /i9 +
       /i8 & i9
o16.oe = vcc

/o17 = /i3 & /i9 +
       /i7 & i9
o17.oe = vcc

/o18 = /i2 & /i9 +
       /i6 & i9
o18.oe = vcc

/o19 = /i1 & /i9 +
       /i5 & i9
o19.oe = vcc
```

### WI01P056.U56.eqn

```
Inputs:

2, 3, 4, 5, 6, 7, 8, 9, 12, 13, 14, 15, 16, 17, 18, 19

Outputs:

14 (Registered, Output feedback registered, Active high)
15 (Registered, Output feedback registered, Active high)
16 (Registered, Output feedback registered, Active high)
17 (Registered, Output feedback registered, Active low)
18 (Registered, Output feedback registered, Active low)
19 (Registered, Output feedback registered, Active high)

Equations:

rf14 := /i7 & rf14 +
        rf14 & rf18 +
        i7 & /rf19
rf14.oe = OE

rf15 := /i4 & /i5 & i6 & i7 +
        rf15 & rf17 +
        /i5 & /i6 & /rf17 +
        /i4 & /i5 & /rf17 +
        i2 & i3 & /i5 & /rf17
rf15.oe = OE

rf16 := i2 & /i3 & i4 & /i5 & i6 & /rf17 +
        /i3 & i4 & /i5 & i6 & rf14 & /rf17 +
        /i3 & i4 & /i5 & i6 & rf14 & /rf15 +
        /i5 & /i6 & /rf18 +
        i2 & i4 & /i5 & /rf18 +
        /i4 & /i5 & /i7 & /rf18 +
        i2 & /i3 & i4 & /i5 & i6 & /rf19
rf16.oe = OE

/rf17 := /i5 & /i6 & /rf17 +
         /i4 & /i5 & /rf17 +
         i2 & i3 & /i5 & /rf17 +
         i7 & rf15 & rf17
rf17.oe = OE

/rf18 := i2 & /i3 & i4 & /i5 & i6 & /rf17 +
         /i3 & i4 & /i5 & i6 & rf14 & /rf17 +
         /i5 & /i6 & /rf18 +
         i2 & i4 & /i5 & /rf18 +
         /i3 & i4 & /i5 & i6 & rf14 & /rf15 +
         /i4 & /i5 & /i7 & /rf18 +
         i2 & /i3 & i4 & /i5 & i6 & /rf19
rf18.oe = OE

rf19 := /i4 & /i5 & i6 & i7 +
        i2 & /i3 & i4 & /i5 & i6 +
        /i3 & i4 & /i5 & i6 & rf14 +
        i2 & /i5 & rf19 +
        /i4 & /i5 & rf19 +
        /i5 & /i6 & rf19 +
        rf15 & rf17 & rf18 & rf19
rf19.oe = OE
```

### WI01P058.U58.eqn

```
Inputs:

2, 3, 4, 5, 6, 7, 8, 9, 12, 13, 14, 15, 16, 17, 18, 19

Outputs:

13 (Registered, Output feedback registered, Active low)
14 (Registered, Output feedback registered, Active low)
15 (Registered, Output feedback registered, Active low)
16 (Registered, Output feedback registered, Active low)
17 (Registered, Output feedback registered, Active low)

Equations:

/rf13 := rf16 & /rf17 +
         /i2 & i3 & rf14 & rf16
rf13.oe = OE

/rf14 := /rf14 & rf15 & rf16 & rf17 +
         /i2 & /i3 & /i4 & /i5 & i6 & /i8 & /i12 & /i19
rf14.oe = OE

/rf15 := rf15 & /rf16 & rf17 +
         /i2 & i3 & rf14 & rf16 & rf17
rf15.oe = OE

/rf16 := rf15 & /rf16 +
         rf16 & /rf17 +
         /i2 & i3 & rf14 & rf16
rf16.oe = OE

/rf17 := rf16 & /rf17 +
         i8 & /rf14 & rf16 +
         /i2 & i3 & rf14 & rf16 +
         /i2 & /i4 & /i5 & i6 & i8 & rf16 +
         /i2 & /i3 & /i4 & /i5 & i6 & i12 & rf16 +
         /i2 & /i3 & /i4 & /i5 & i6 & rf16 & i19
rf17.oe = OE
```

### WI01P067.U67.eqn

```
Inputs:

2, 3, 4, 5, 6, 7, 8, 9, 12, 13, 14, 15, 16, 17, 18, 19

Outputs:

12 (Combinatorial, Output feedback output, Active low)
13 (Combinatorial, Output feedback output, Active low)
14 (Registered, Output feedback registered, Active low)
15 (Registered, Output feedback registered, Active low)
16 (Registered, Output feedback registered, Active low)
17 (Registered, Output feedback registered, Active low)
18 (Combinatorial, Output feedback output, Active low)
19 (Combinatorial, Output feedback output, Active low)

Equations:

/o12 = i5 & /rf14 +
       i4 & /i5
o12.oe = vcc

/o13 = i5 & /rf15 +
       i4 & /i5
o13.oe = vcc

/rf14 := i2 & i3
rf14.oe = OE

/rf15 := /i2 & i3
rf15.oe = OE

/rf16 := i2 & /i3
rf16.oe = OE

/rf17 := /i2 & /i3
rf17.oe = OE

/o18 = i5 & /rf16 +
       i4 & /i5
o18.oe = vcc

/o19 = i5 & /rf17 +
       i4 & /i5
o19.oe = vcc
```

### WI01P070.U70.eqn

```
Inputs:

2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23

Outputs:

15 (Combinatorial, Output feedback output, Active high)
16 (Combinatorial, Output feedback output, Active high)
17 (Registered, Output feedback registered, Active low)
18 (Registered, Output feedback registered, Active high)
19 (Registered, Output feedback registered, Active high)
20 (Registered, Output feedback registered, Active high)

Equations:

o15 = /i2 & /i4 & i7 & i8 +
      /i4 & /i7 & /i8 & /i9 & /i10 & i11 & /i14 & i21 & i22 & /i23
o15.oe = vcc

o16 = /i2 & /i3 & /i4 & /i5 & i7 & i8 & rf20 +
      /i2 & /i3 & /i4 & /i5 & i7 & i8 & /rf19 +
      /i2 & /i3 & /i4 & /i5 & i7 & i8 & rf18 +
      /i3 & /i4 & /i5 & /i7 & /i8 & /i9 & /i10 & i11 & /i14 & rf20 & i21 & i22 & /i23 +
      /i3 & /i4 & /i5 & /i7 & /i8 & /i9 & /i10 & i11 & /i14 & /rf19 & i21 & i22 & /i23 +
      /i3 & /i4 & /i5 & /i7 & /i8 & /i9 & /i10 & i11 & /i14 & rf18 & i21 & i22 & /i23
o16.oe = vcc

/rf17 := rf18 +
         /i2 & /i3 & /i4 & /i5 & i6 & i7 & i8 & /rf19 +
         /i3 & /i4 & /i5 & i6 & /i7 & /i8 & /i9 & /i10 & i11 & /i14 & /rf19 & i21 & i22 & /i23
rf17.oe = OE

rf18 := rf18 & /rf19 +
        /i2 & /i3 & /i4 & /i5 & i6 & i7 & i8 & /rf19 +
        /i3 & /i4 & /i5 & i6 & /i7 & /i8 & /i9 & /i10 & i11 & /i14 & /rf19 & i21 & i22 & /i23
rf18.oe = OE

rf19 := rf18 +
        /i3 & rf19
rf19.oe = OE

rf20 := 
rf20.oe = OE
```

### WI01P072.U72.eqn

```
Inputs:

2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23

Outputs:

15 (Registered, Output feedback registered, Active low)
16 (Registered, Output feedback registered, Active low)
17 (Combinatorial, Output feedback output, Active low)
18 (Combinatorial, Output feedback output, Active low)
19 (Combinatorial, Output feedback output, Active high)
20 (Combinatorial, Output feedback output, Active high)
21 (Combinatorial, Output feedback output, Active high)
22 (Combinatorial, Output feedback output, Active high)

Equations:

/rf15 := /i23
rf15.oe = OE

/rf16 := /i14
rf16.oe = OE

/o17 = i8 & /i9
o17.oe = vcc

/o18 = i7 & /i9
o18.oe = vcc

o19 = i2 & /i6 & /i10 +
      i2 & /i6 & i11
o19.oe = vcc

o20 = i3 & /i6 & /i10 +
      i3 & /i6 & i11
o20.oe = vcc

o21 = i4 & /i6 & /i10 +
      i4 & /i6 & i11
o21.oe = vcc

o22 = i5 & /i6 & /i10 +
      i5 & /i6 & i11
o22.oe = vcc
```

### WI01P073.U73.eqn

```
Inputs:

1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 14, 15, 16, 17, 20, 21, 22, 23

Outputs:

15 (Combinatorial, Output feedback output, Active low)
17 (Combinatorial, Output feedback output, Active low)
18 (Combinatorial, No output feedback, Active low)
19 (Combinatorial, No output feedback, Active low)
20 (Combinatorial, Output feedback output, Active low)
21 (Combinatorial, Output feedback output, Active low)
22 (Combinatorial, Output feedback output, Active low)

Equations:

/o15 = i1 & i8 & i9 & i10 & /i23
o15.oe = vcc

/o17 = 
o17.oe = vcc

/o18 = 
o18.oe = vcc

/o19 = i2 & /i5 & i6 & /i7 & /i8 & i14 +
       i8 & i9 & i14 & /i23 +
       i2 & /i4 & /i7 & /i8 & i14 +
       /i3 & i4 & /i7 & /i8 & i14 +
       i8 & i9 & i11 & /i23
o19.oe = vcc

/o20 = i2 & /i5 & i6 & /i7 & /i8 & /i14 +
       i8 & i9 & /i14 & /i23 +
       i2 & /i4 & /i7 & /i8 & /i14 +
       /i3 & i4 & /i7 & /i8 & /i14 +
       i8 & i9 & i11 & /i23
o20.oe = vcc

/o21 = i1 & i8 & i14 & /i23 +
       i1 & i2 & /i5 & i6 & i7 & /i8 & i14 +
       i1 & /i3 & /i5 & i6 & i7 & /i8 & i14 +
       i1 & i8 & i11 & /i23 +
       i1 & i2 & /i4 & i7 & /i8 & i14 +
       i1 & /i3 & i4 & i7 & /i8 & i14
o21.oe = vcc

/o22 = i1 & i8 & /i14 & /i23 +
       i1 & i2 & /i5 & i6 & i7 & /i8 & /i14 +
       i1 & /i3 & /i5 & i6 & i7 & /i8 & /i14 +
       i1 & i8 & i11 & /i23 +
       i1 & i2 & /i4 & i7 & /i8 & /i14 +
       i1 & /i3 & i4 & i7 & /i8 & /i14
o22.oe = vcc
```

### WI01P074.U74.eqn

```
Inputs:

2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23

Outputs:

15 (Registered, Output feedback registered, Active low)
16 (Registered, Output feedback registered, Active low)
17 (Registered, Output feedback registered, Active low)
18 (Registered, Output feedback registered, Active low)
19 (Registered, Output feedback registered, Active low)
20 (Registered, Output feedback registered, Active low)
21 (Registered, Output feedback registered, Active low)
22 (Registered, Output feedback registered, Active low)

Equations:

/rf15 := /i2 & /i3 +
         /i2 & /i9 & /i11 +
         /i2 & i6 & /i8 & /i11 +
         /i2 & /i7 & /i8 & /i11 +
         /i2 & /i5 & i6 & /i7 & /i11 +
         /i2 & /i4 & /i5 & i6 & /i11
rf15.oe = OE

/rf16 := /i2 & /i3 +
         /i2 & /i9 & /i10 +
         /i2 & i6 & /i8 & /i10 +
         /i2 & /i7 & /i8 & /i10 +
         /i2 & /i5 & i6 & /i7 & /i10 +
         /i2 & i4 & /i5 & i6 & /i10
rf16.oe = OE

/rf17 := /i2 & /i3 +
         /i2 & /i9 & /i23 +
         /i2 & i6 & /i8 & /i23 +
         /i2 & /i7 & /i8 & /i23 +
         /i2 & i5 & i6 & /i7 & /i23 +
         /i2 & /i4 & i5 & i6 & /i23
rf17.oe = OE

/rf18 := /i2 & /i3 +
         /i2 & /i9 & /i14 +
         /i2 & i6 & /i8 & /i14 +
         /i2 & /i7 & /i8 & /i14 +
         /i2 & i5 & i6 & /i7 & /i14 +
         /i2 & i4 & i5 & i6 & /i14
rf18.oe = OE

/rf19 := /i2 & /i3 +
         /i2 & /i9 & /i11 +
         /i2 & /i6 & /i8 & /i11 +
         /i2 & /i7 & /i8 & /i11 +
         /i2 & /i5 & /i6 & /i7 & /i11 +
         /i2 & /i4 & /i5 & /i6 & /i11
rf19.oe = OE

/rf20 := /i2 & /i3 +
         /i2 & /i9 & /i10 +
         /i2 & /i6 & /i8 & /i10 +
         /i2 & /i7 & /i8 & /i10 +
         /i2 & /i5 & /i6 & /i7 & /i10 +
         /i2 & i4 & /i5 & /i6 & /i10
rf20.oe = OE

/rf21 := /i2 & /i3 +
         /i2 & /i9 & /i23 +
         /i2 & /i6 & /i8 & /i23 +
         /i2 & /i7 & /i8 & /i23 +
         /i2 & i5 & /i6 & /i7 & /i23 +
         /i2 & /i4 & i5 & /i6 & /i23
rf21.oe = OE

/rf22 := /i2 & /i3 +
         /i2 & /i9 & /i14 +
         /i2 & /i6 & /i8 & /i14 +
         /i2 & /i7 & /i8 & /i14 +
         /i2 & i5 & /i6 & /i7 & /i14 +
         /i2 & i4 & i5 & /i6 & /i14
rf22.oe = OE
```

### WI01P075.U75.eqn

```
Inputs:

2, 3, 4, 5, 6, 7, 8, 9, 12, 13, 14, 15, 16, 17, 18, 19

Outputs:

14 (Registered, Output feedback registered, Active low)
15 (Registered, Output feedback registered, Active low)
16 (Registered, Output feedback registered, Active low)
17 (Registered, Output feedback registered, Active low)

Equations:

/rf14 := /i2 & i6 +
         /i2 & /i3 +
         /i2 & i4 & i5
rf14.oe = OE

/rf15 := /i2 & i6 +
         /i2 & /i3 +
         /i2 & i4 & i5
rf15.oe = OE

/rf16 := /i2 & /i6 +
         /i2 & /i3 +
         /i2 & i4 & i5
rf16.oe = OE

/rf17 := /i2 & /i6 +
         /i2 & /i3 +
         /i2 & i4 & i5
rf17.oe = OE
```

### WI01P079.U79.eqn

```
Inputs:

2, 3, 4, 5, 6, 7, 8, 9, 12, 13, 14, 15, 16, 17, 18, 19

Outputs:

17 (Registered, Output feedback registered, Active high)
18 (Registered, Output feedback registered, Active low)
19 (Combinatorial, Output feedback output, Active low)

Equations:

rf17 := i2
rf17.oe = OE

/rf18 := /i9 & /rf17
rf18.oe = OE

/o19 = i4 & /i7 & i8
o19.oe = vcc
```

### WI01P080.U80.eqn

```
Inputs:

2, 3, 4, 5, 6, 7, 8, 9, 12, 13, 14, 15, 16, 17, 18, 19

Outputs:

12 (Combinatorial, Output feedback output, Active low)
14 (Registered, Output feedback registered, Active low)
15 (Registered, Output feedback registered, Active low)
16 (Registered, Output feedback registered, Active low)
17 (Registered, Output feedback registered, Active low)
18 (Combinatorial, Output feedback output, Active low)
19 (Combinatorial, Output feedback output, Active low)

Equations:

/o12 = /i2 & /i9 +
       /i2 & i7 & /i8 & /rf14 & /rf15 & /rf16 & /rf17
o12.oe = vcc

/rf14 := /i6 & /o18 +
         /i8 & i9 & /rf14 & /rf16 & o18 +
         /i8 & i9 & /rf14 & /rf15 & o18 +
         /i8 & i9 & /rf14 & /rf17 & o18 +
         /i8 & i9 & rf14 & rf15 & rf16 & rf17 & o18
rf14.oe = OE

/rf15 := /i5 & /o18 +
         /i8 & i9 & /rf15 & /rf16 & o18 +
         /i8 & i9 & /rf15 & /rf17 & o18 +
         /i8 & i9 & rf15 & rf16 & rf17 & o18
rf15.oe = OE

/rf16 := /i4 & /o18 +
         /i8 & i9 & rf16 & rf17 & o18 +
         /i8 & i9 & /rf16 & /rf17 & o18
rf16.oe = OE

/rf17 := /i3 & /o18 +
         /i8 & i9 & rf17 & o18
rf17.oe = OE

/o18 = /i9 +
       i8 +
       /rf14 & /rf15 & /rf16 & /rf17
o18.oe = vcc

/o19 = /i2 & /i8 & /rf14 & /rf15 & /rf16 & /rf17
o19.oe = vcc
```

### WI01P083.U83.eqn

```
Inputs:

2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23

Outputs:

15 (Registered, Output feedback registered, Active low)
16 (Registered, Output feedback registered, Active high)
17 (Registered, Output feedback registered, Active low)
18 (Registered, Output feedback registered, Active low)
19 (Registered, Output feedback registered, Active low)
20 (Registered, Output feedback registered, Active low)
21 (Registered, Output feedback registered, Active low)
22 (Registered, Output feedback registered, Active low)

Equations:

/rf15 := /rf15 & /rf20 & rf21 +
         /rf15 & /rf18 & rf21 +
         /rf15 & /rf19 & rf21 +
         /i6 & rf18 & rf19 & rf20 & rf21
rf15.oe = OE

rf16 := rf18 & rf19 & rf20 & /rf21 & rf22 +
        rf18 & /rf19 & /rf20 & /rf21 +
        /rf15 & /rf18 & /rf20 & rf21 +
        rf19 & /rf20 & rf21 +
        rf18 & /rf19 & /rf22 +
        rf15 & /rf18 & rf19 & rf21 +
        i6 & rf18 & rf19 & rf21
rf16.oe = OE

/rf17 := rf19 & /rf21 +
         /rf18 & rf20 & rf22 +
         rf18 & /rf19 & rf22 +
         /rf21 & rf22 +
         i6 & /i11 & rf19 & rf20 +
         /i3 & i4 & rf15 & /rf18 & rf22 +
         /i2 & /i5 & i6 & /i7 & i8 & rf19 & rf20
rf17.oe = OE

/rf18 := /rf19 & rf20 & /rf21 & rf22 +
         rf19 & /rf20 & /rf21 +
         /i6 & rf19 & rf20 & rf21 +
         /rf18 & rf19 & rf20 & rf21 +
         /i11 & rf19 & rf20 & rf21 +
         rf19 & /rf22 +
         /rf19 & /rf20 & rf21 +
         /i2 & /i5 & /i7 & i8 & rf19 & rf20 & rf21
rf18.oe = OE

/rf19 := /rf18 & /rf20 & /rf21 +
         /rf18 & /rf22 +
         /rf18 & rf20 & rf21 +
         /rf15 & rf18 & /rf20 & rf21 +
         rf18 & /rf19 & rf20 +
         rf18 & /rf19 & rf22
rf19.oe = OE

/rf20 := rf19 & /rf20 & /rf21 +
         /rf18 & /rf20 +
         rf18 & /rf19 & rf21 +
         rf18 & /rf19 & rf20 & /rf22 +
         /rf15 & rf19 & /rf20
rf20.oe = OE

/rf21 := /rf21 & rf22 +
         /rf15 & rf18 & rf19 & /rf20 +
         /i3 & i4 & rf15 & /rf18 & rf19 & /rf20 +
         rf20 & /rf21
rf21.oe = OE

/rf22 := rf18 & rf19 & rf20 & /rf21 +
         rf20 & /rf22 +
         /rf15 & rf18 & rf19 & /rf20 & rf21
rf22.oe = OE
```

### WI01P092.U92.eqn

```
Inputs:

2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23

Outputs:

15 (Registered, Output feedback registered, Active low)
16 (Registered, Output feedback registered, Active low)
17 (Registered, Output feedback registered, Active low)
18 (Registered, Output feedback registered, Active low)
19 (Registered, Output feedback registered, Active low)
20 (Registered, Output feedback registered, Active high)
21 (Registered, Output feedback registered, Active low)
22 (Registered, Output feedback registered, Active low)

Equations:

/rf15 := 
rf15.oe = OE

/rf16 := /i8
rf16.oe = OE

/rf17 := i2 & /i5 +
         i2 & /i4 +
         /i3 & i4
rf17.oe = OE

/rf18 := i2 & i3 & /i4 & /i5 & i6 & i9
rf18.oe = OE

/rf19 := i2 & /i4 & /i5 & /i6 & i9 +
         /i2 & /i3 & i4 & /i5 & /i6 & i9 +
         i2 & /i3 & i4 & i5 & /i6 & i9
rf19.oe = OE

rf20 := rf16 & rf20 +
        i2 & i3 & /i4 & i5 & i6 & /i9 & rf16
rf20.oe = OE

/rf21 := /i2 & /rf21 +
         /i3 & /rf21 +
         /i4 & /rf21 +
         i5 & /rf21 +
         i6 & /rf21 +
         /i9 & /rf21 +
         /i7 & /i10 & i11 & /i14
rf21.oe = OE

/rf22 := 
rf22.oe = OE
```

