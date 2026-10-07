// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef DOSBOX_PROGRAM_XRCFG_H
#define DOSBOX_PROGRAM_XRCFG_H

#include "dos/programs.h"

#include <string>
#include <vector>

class XRCFG final : public Program {
public:
	XRCFG()
	{
		help_detail = {HELP_Filter::All,
		               HELP_Category::Misc,
		               HELP_CmdType::Program,
		               "XRCFG"};
	}
	void Run() override;
};

// Implemented in gui/sdl_gui.cpp (XR preset files using xr_ key names).
bool OPENXR_SaveConfig(const std::string& name);
bool OPENXR_LoadConfig(const std::string& name);
std::vector<std::string> OPENXR_ListConfigs();

#endif
