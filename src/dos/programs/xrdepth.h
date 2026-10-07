// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef DOSBOX_PROGRAM_XRDEPTH_H
#define DOSBOX_PROGRAM_XRDEPTH_H

#include "dos/programs.h"

class XRDEPTH final : public Program {
public:
	XRDEPTH()
	{
		help_detail = {HELP_Filter::All,
		               HELP_Category::Misc,
		               HELP_CmdType::Program,
		               "XRDEPTH"};
	}
	void Run() override;
};

#endif
