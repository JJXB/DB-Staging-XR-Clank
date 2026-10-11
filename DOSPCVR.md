# DOS/PC VR from the ibiblio `virtual-reality` Mirror — Infodump

Source tree surveyed: `D:\Emu\DOS\C\VRDRV\vrref\www.ibiblio.org\pub\academic\
computer-science\virtual-reality\` (HTTrack mirror of
`https://www.ibiblio.org/pub/academic/computer-science/virtual-reality/`,
11 Oct 2026; 1208 links scanned, 1112 files; `.Z` compress files failed to
transfer and are noted but not decoded).
Every area below was opened via its local `index.html` plus the named
representative files. Sibling DOS/VR trees consulted for cross-refs:
`D:\Emu\DOS\C\VRDRV\develop\` (CyberShades SDK), `PCVRJET\` (PCVR Jet),
`VFX1\` + `vfx1sdk\` + `FVFX1B\` + `vfx1drivers\` (Forte VFX-1), `zip\`.

Scope: **everything in that mirror a DOS/PC programmer or emulator author
cares about** — DOS APIs, parallel/serial port protocols, file formats,
hardware wiring, and which archives hold runnable DOS VR code. SGI/NeXT/
Amiga/Mac/IRC/FAQ material is one-lined as out of scope. Complements
`SU2000.md` (Virtuality arcade, not in this mirror), `VFX1.md`,
`cybershades.md`, `VMaxxCyMaxx.md`. Nothing below comes from emulator
sources.

## 1. The 30-second version

1. This mirror is **1991–1994 sci.virtual-worlds-era**: REND386 DOS VR
   library + PowerGlove/UForce parallel-port hacks + a few DOS VR demos
   (SuperScape, VRS/VR Studio, 3D viewers) + papers/standards. No Virtuality
   SU-2000, no VFX-1, no CyberMaxx inside — those live in the sibling trees.
2. **REND386** (`rend386/`) is the DOS VR workhorse: 386/486 + VGA, Borland
   C++ 2.0 / TASM, `.PLG/.WLD/.FIG` worlds, `VD16/VD256.RVD` display drivers,
   `rend386.cfg`, `OFF→PLG`-family converters. Frozen May 1 1994
   (successors: AVRIL, VR-386). Runnable DOS VR apps ship here:
   `pcvrjet.exe`, `racball.exe` (PowerGlove), `pgsi5.exe` (serial interface).
3. **PowerGlove on DOS** (`powerglove/ibm/`) is fully specified: parallel
   bit-bang on **`0x378/0x379`** (`GDATA 0x10`, `GLATCH 0x02`,
   `GCLOCK 0x01`), wiring `DB25 2→glove 2, 3→3, 13←4, 18→1, +5V→7` (BYTE
   July 1990 pp. 288–289), Turbo C 2.0 `ibmpc-hires` 12-byte hires packet
   `A0 X Y Z rot finger keys 00 00 3F FF FF`, plus PGSA-II serial command
   set (`powerglove.data.sheet`). UNIX `power-386/` sources are timing refs.
4. **UForce on DOS-pattern** (`uforce/Uforce/`) documents the analog NES-pad
   protocol (`reader`/`lores`, `query_NES()`/`query_Uforce()`, 150 µs
   inter-byte, `00`=acquiring, `FC–FF` flags) — Amiga C but the CIAA bits map
   1:1 onto PC LPT `0x378/0x379`.
5. **DOS VR demos** (`ibm/`): `3dv25.zip` wireframe viewer, `sscape.zip`
   SuperScape, `VRSdemo.zip` VR Studio (`RUNVGA.EXE`), `vectdemo.zip` is
   **demoscene, not VR** (UltraForce NL + TRSI warez NFO), plus `cyberfun.exe`
   SVGA 640×480×256 demo and `1wolf3d.zip`.
6. **Papers/standards of DOS interest**: `standards/` OFF spec
   (`off.ms.Z/off.ps.Z`) + `5d.vr.os.Z`; `papers/` `$5-a-day VR`,
   `vr.primer`, `whatisvr.txt` (FTP-site guide pointing back at this
   archive), `w.industries.presentation.paper` (Virtuality arcade rollout),
   `Lee.ImplementingVR`.

## 2. Mirror layout (root `index.html` → `INDEX`)

Root dirs: `general/ glove-list/ ibm/ irc/ papers/ powerglove/ rend386/
sgi/ standards/ uforce/ virtual-worlds/`. Root `INDEX` (1993-12-23, = excerpt
of `general/vr_ftp.list`): `ibm=MSDOS`, `rend386=Mirror of
sunee.uwaterloo.ca`, plus `3d/general/glove-list/irc/sgi/papers/powerglove/
standards/uforce/virtual-worlds/communications`.

## 3. REND386 (`rend386/`) — the DOS VR library

Identity: Dave Stampe + Bernie Roehl fast 3D renderer, Releases 4/5.
`read.me` (1994-04-28): *"no longer supported as of May 1 1994 — use AVRIL
(broehl@uwaterloo.ca) or VR-386 (dstampe@psych.utoronto.ca)"*.
`oldread.me`: 386/486 only (no 286), VGA, Turbo C++ 2.00+/Borland C, TASM.

- Top files: `demo4.zip` 126K, `devel4.zip` 206K, `devel5.zip` 236K,
  `drkit4.zip` 163K, `unzip.exe` 30K, `read.me`, `oldread.me`,
  `list-archive.mht` 1.8M.
- Subdirs: `applications/ converters/ demos/ floatsrc/ irit/ ogre/ old/
  pgsi/ plgfiles/ polyblit/ utils/ versions/ worlds/`.
- `versions/devel4-cl.txt` (Charlie Lindahl 7/29/93): `WORLD.C` BCC 2.0
  `SELECT` fix, BCC makefiles, Menelli-box `SEGA/` support, `PGSI serial →
  REND386` hack notes.
- `applications/read.me`: `racball.exe` (self-extract, PowerGlove
  racquetball, PCVR), `pcvrjet.exe` (self-extract), `btmate2.zip` (Gradecki
  2-player chess).
- `converters/read.me` (all → `.PLG`): `3ds2plg/byu2plg/dxf2plg+dxfs3/
  irit2plg/off2plg/srf2plg/shd2plg/plgfilt/combvert/wld2vrml`.
- `worlds/*.zip`: `ackwin/car/dzzyland/fair/figures/kingme/scuba/valley +
  wlds.txt`; `utils/*.zip`: `checker.exe/formla/globe/iegeom/mobius/
  np-demo/ogre41/plgstars/prism/soccer`; `demos/`: `2mrobo/fvt/inrwsr/
  intrwl/mtxsrc/relax/vrart`; `pgsi/pgsi5.exe + pgsisega.zip` = **PowerGlove
  Serial Interface DOS**.
- Programmer surface: `#include "rend386.h"` (`PCVRJET/JET.C` pattern),
  `rend386.cfg`, worlds `.WLD`, objects `.PLG`, figures `.FIG`, drivers
  `.RVD` (`VD16.RVD/VD256.RVD` Dave Stampe 19/8/92) + `MOUSEDRV.RPD`,
  Borland `.DSK/.PRJ`. OFF spec lives in `standards/` (see §6).

## 4. PowerGlove (`powerglove/` → `powerglove/ibm/`)

Top: `NeXT/ amiga/ hc11/ ibm/ mac/ O2GLOVE.TXT o2glove.zip pg.power
pglove.faq.sep.1993 pgsa.doc pgtimg.gif powerglove-faq.texinfo/txt.gz
powerglove.data.sheet powerglove.faq.gz`.
`pg.power` == `ibm/how.to.hook.up.a.powerglove` (Bill wgerlt@atl.ge.com
12/20/91). `pglove.faq.sep.1993` (J.E. Townsend, sci.virtual-worlds 9904).
`powerglove.data.sheet` (Jim Brain 22-Apr-92): **PGSA-II Serial Adapter**.

### 4a. DOS parallel wiring + hires code (the emulator-relevant part)
`ibm/`: `386-pg.mht handsrc.zip how.to.hook.up.a.powerglove ibmpc-hires
pghand2.zip power-386.sh power-386/ thehand.zip`.
Wiring (both docs, ref BYTE July 1990): `DB25 pin2 clock → glove pin2,
pin3 latch → pin3, pin13 data ← pin4, pin18 GND → pin1, 5V → pin7`
(external 7805 + 2×0.1 mF from 7–9 V, or disk-Molex 5V/GND, or keyboard
pin5/4).
`ibmpc-hires` (manfredo@opal.cs.tu-berlin.de 9/91, PC port Greg Alt 10/91,
**Turbo C 2.0**, `N=4/D=21` for 386/25): **DOS `outportb(0x378)/
inportb(0x379)` bit-bang** (`GDATA 0x10 / GLATCH 0x02 / GCLOCK 0x01`,
`GCLOLAT 0x03`), `Hires()` init dance + `getglove()` 12-byte packet
`A0 X Y Z rot finger keys 00 00 3F FF FF` (delays `D2BYTES 192 / D2BITS 21 /
D2SLOW 32000`). Full 270-line source mirrored locally — port constants and
packet layout above are the contract to emulate.

### 4b. Serial alternative + UNIX timing ref
`powerglove.data.sheet`: Minelli emu (`C` continuous / `R` request / `?`
poll / `+` deglitch / `-` off, 7 B `A0 X Y Z rot flex switch`, request mode
6 B no `A0`); AGE emu (`0x01` cont / `0x02` req / `0x04` reinit / `0x07`
reset / `0x08–09` rot-filter, 11 B `5F 5F X Y Z rot flex switch GSTAT1/2`);
`flex` 2 bit × 4 fingers; `0x0A` baud / `0x11–1A` 2nd glove /
`0x0B–0D` Sega glasses; max 9600 (hope 38400).
`powerglove/ibm/power-386/` (Lance Norskog, UNIX V.3/386 Streams + X386:
`nes.c/strobe.c/mouseX386.c`, `ctrl 0x378 bits 1/2, data 0x379 mask 0x10`,
`nes_c1-5=20` @20 MHz, `ioctl KDADDIO 0x378/379/37a`): **not DOS**, useful
as timing/pinout reference only.

## 5. UForce (`uforce/` → `uforce/Uforce/`)

`Uforce_v1.00.lzh` 20K + `Uforce/`: `NES.c/h Uforce.c/h reader.c lores.c
makefile timer.c/h read.me reader.html lores.html` + `.o`.
`read.me` (Ethan Dicks 24-Mar-92): Amiga code, same cable as Amiga
PowerGlove hires hack. `lores` = generic NES-pad byte reader; `reader` =
UForce analog sampler.
`Uforce.c` (from Dave Capper/Stan Axelrod guide, no NDA): 9
emitters/detectors (8 active); switches 1–3 mode, switch 4 sensor-5 select;
7 threshold modes → std 8-bit NES pad; `111b` = analog 9 B
(`Flag S7..S0`, `Flag FC–FF`: `FF` none / `FE` START / `FD` SELECT / `FC`
both; samples `01–FB`; `00` = acquiring ≤10 ms discard; 150 µs inter-byte;
std `query_NES()` works unlike PG hires).
`Uforce.h`: `U_FLAG_HI 255/LO 252, U_SELECT 2/START 1, SAMPLE_HI 251/LO 1,
SAMPLING 0`, `typedef {f0,a0..a7} Uforce_data`. `NES.c` (Atari ST manfredo
→ Amiga Alan Bland `query_NES()`: `GDATA 0x04/GLOCK 0x01/GLATCH 0x02` on
CIAA, `RESET L-H-L ≥4 µs`, 8× `clock L-H-L ≥3 µs`): port to DOS by
substituting CIAA with LPT `0x378/0x379`.

## 6. DOS VR demos + utilities (`ibm/`)

`read.me`: `3dv25.zip` wireframe viewer / `sscape.zip` SuperScape /
`vectdemo.zip` UltraForce / `VRSdemo.zip` VR Studio.
- `3dv25.zip` 72K + `3dv25/`: `3DV.EXE + 3DV.DOC + *.3D/*.3DV`
  (2SKULLS/GALAXY/GLASS/HEIGHTS/HOUSE/SHUTTLE/SKULL/SOLIDS/SURFACE/TREE1).
- `VRSdemo.zip` 49K + `VRSdemo/`: `RUNVGA.EXE + VRS.DOC + DEMO.RUN`.
- `sscape.zip` 98K (+ `superscape.tar.Z` 137K, failed `.Z` transfer) +
  `superscape.dir/`: `DEMO.EXE/DOC + DEMO.WLD/.SHP/.PAL/.LBM + SS.CFG/.LBM`.
- `vectdemo.zip` 215K + `vectdemo/`: `VECTDEMO.EXE + ORDER.FRM + VECT_*.U4C
  + YEPO.NFO (TRSI warez)` — **demoscene, not VR** (UltraForce NL PD order:
  $6 SB collection, Rotterdam P.O.).
- Also: `1wolf3d.zip` 637K, `cyberfun.exe` 1.0M + `cyberfun.readme` (SVGA
  640×480×256, `mkdir CYBER; cyberfun; demo`, 256K VRAM + vendor `.COM`
  SVGA driver, `whichvga` check), `vrstudio.archive…removed.html` stub.

## 7. Standards + papers (DOS/PC-relevant only)

`standards/`: `5d.vr.os.Z` 44K / `off.ms.Z` 17K / `off.ps.Z` 40K / `read.me`
(`off.*` = troff/PostScript **OFF object-file-format spec** — the format
`off2plg` converts; `5d.vr.os.Z` = 5D VR-OS text, `.Z` not expanded).
`papers/` (22 entries): DOS-relevant compressed (`.Z`, not expanded):
`vr.on.five.dollars.a.day.Z` 12K, `vr.primer.Z` 8.4K,
`general.vr.article.Z`, `Houstn.article.Z`; plain: `whatisvr.txt` 74K +
`.zip` (Jerry Isdale homebrew intro, §9.3 = FTP guide pointing at this very
archive), `Lee.ImplementingVR` 42K, `w.industries.presentation.paper` 24K
(**W Industries/Virtuality arcade rollout** — the only Virtuality hit in the
mirror), `electropolis/slice.of.life/DIAC92` (MOO/IRC culture, not DOS).

## 8. The rest (one line each — not DOS/PC)

- `sgi/`: Iris GL games/grafix/tutorials only.
- `glove-list/`: `gla-91*.Z` + 406K mbox — PowerGlove mail, platform-agnostic.
- `irc/`: empty stub (moved to `/pub/academic/communications`).
- `virtual-worlds/`: `faq.tar` 2.9M + `faq/postings/citations` =
  sci.virtual-worlds mirror.
- `general/`: `veos.tar.Z` 549K (failed transfer), `vr_ftp.list` 8.3K (Toni
  Emerson HITLab guide: `sunee pub/vr/demos wireframe/SuperScape/UltraForce/
  REND386/VR Studio`), `vr_sites` (Bill Cockayne: `sunee.uwaterloo.ca
  /pub/rend386`, `karazm.math.uh.edu /pub/VR`, `sunsite.unc.edu…`),
  `sites.of.virtuality` 6.2K (Bob Jacobson 21-Dec-91: **VIRTUALITY 1000SD/
  1000CS arcade US rollout** — arcade, not DOS), `AmigaGun.lha`,
  `general/images/`.

## 9. Sibling-tree cross-ref (where the real DOS/VR drivers live)

The mirror is **not** where VFX-1/CyberShades/PCVRJet code lives — those are
next to it in `D:\Emu\DOS\C\VRDRV\` (already covered by `VFX1.md` /
`cybershades.md`, noted here so nobody hunts the mirror for them):
- `develop/` (53 files): **Chinon CyberShades SDK Beta1 11/8/95**
  (`CYSHSDK.TXT CSHADE/VTIMER/VRI/LCDCALL/GFX/VGFX/… GRIDEMO/TEST/JOYSTICK/
  ISRSTUB DOS4GW.EXE LCDBIOS.EXE`, Watcom 9.5b 386-flat).
- `PCVRJET/PCVRJET/` (48 files): **PCVR Jet REND386+PG+Sega** (`JET.C`
  `#include rend386.h` + `JET.EXE/DOC/WLD/PLG/FIG/VD16/VD256.RVD`).
- `VFX1/` (42 files): IIS VFX-1 DOS utils 2.16 (`VFX1.COM SETVFX1.EXE
  VFX1DIAG.EXE VRMOUSE.COM/INI VR*.BAT FIELDPRG/IFCAL`).
- `vfx1sdk/`: Forte SDK (`VFX1SDK.EXE vfx1_213.exe DOSSDK/QSTART/EXAMPLE1/2
  doc/` serial-protocol + tracker docs).
- `FVFX1B/`: Forte VFX-1 bonus bundle; `vfx1drivers/`: `INSTALL_198.EXE
  vfx1_215.exe`; `zip/`: `cybershades_sdk.zip develop.zip
  forte-vfx-1-bonus.zip vfx_2_16.EXE`.

## 10. Claim → source map

- Mirror stats/`.Z` failures → `vrref\hts-log.txt` (94 errors) + root
  `INDEX`/`general/vr_ftp.list`.
- REND386 req/support-status/files → `rend386/read.me + oldread.me +
  index.html` file listing; app/converter/world lists →
  `rend386/applications|converters|worlds|utils|demos|pgsi/read.me +
  index.html`.
- PG wiring/pins/BYTE ref → `powerglove/ibm/how.to.hook.up.a.powerglove`
  (= `pg.power`) + `powerglove.data.sheet`; DOS bit-bang/packet/delays →
  `powerglove/ibm/ibmpc-hires` full source (§4a constants verbatim).
- UNIX PG timing → `powerglove/ibm/power-386/{READ.ME,nes.c,strobe.c,
  mouseX386.c}`.
- UForce protocol → `uforce/Uforce/{read.me,Uforce.c/h,NES.c/h,reader.c}`.
- Demo identities → `ibm/read.me + index.html` + per-demo dir listings;
  `vectdemo` demoscene verdict → `vectdemo/ORDER.FRM + YEPO.NFO`;
  `cyberfun` req → `ibm/cyberfun.readme`.
- OFF/5D-VR-OS/papers → `standards/{read.me,index.html}`,
  `papers/index.html` + named plain files; Virtuality arcade hits →
  `general/sites.of.virtuality` + `papers/w.industries.presentation.paper`.
