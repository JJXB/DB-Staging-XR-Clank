// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

// One-time LSFG shader extraction.
//
// The Lossless Scaling frame-generation shaders ship as RCDATA resources
// inside the user's own paid copy of Lossless Scaling (Lossless.dll) and
// are not redistributable, so they are extracted on-device, once, and
// cached: the DLL is fingerprinted (FNV-1a64 + size), the blobs are
// written to <config-dir>/lsfg/<fingerprint>/ with a manifest, and later
// runs reuse the cache without touching the DLL again. A Steam update
// that changes the DLL changes the fingerprint, which re-extracts
// automatically -- still zero user action.
//
// The DLL is only ever parsed as data (manual PE resource walk); it is
// never loaded or executed. Expected layout (verified against the user's
// 7.5MB DLL: 300 RCDATA, IDs 101-400): IDs 101-302 are DXBC (base chain),
// IDs 303-400 are precompiled SPIR-V (0x07230203). All blobs are kept;
// the future dispatch stage decides which chain to bind.

#ifndef DOSBOX_LSFG_H
#define DOSBOX_LSFG_H

#include <string>

class SectionProp;

// Register the framegen_lsfg_* keys on the [framegen] section (called
// from FRAMEGEN_InitSection).
void LSFG_AddKeys(SectionProp& section);

// Fingerprint the configured Lossless.dll and stage its shader blobs:
// extract on first use (or first use after a DLL change), reuse the
// cache otherwise. Safe to call every frame; all work happens at most
// once per DLL fingerprint. Returns true when LSFG shaders are staged
// and ready for the future dispatch stage.
bool LSFG_EnsureReady();

// One-line status for logs and (later) OSD, e.g.
// "LSFG staged: 202 DXBC + 98 SPIR-V (cache hit)".
std::string LSFG_StatusText();

// Absolute path of the staged shader cache dir for the current DLL
// fingerprint (empty when nothing is staged yet). The dispatch stage
// loads its SPIR-V blobs from here.
std::string LSFG_StagedDir();

#endif // DOSBOX_LSFG_H
