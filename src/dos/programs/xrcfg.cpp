// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

#include "xrcfg.h"

#include <cstring>
#include <memory>
#include <string>

#include "dos/dos.h"
#include "dos/drive_local.h"
#include "utils/checks.h"
#include "utils/string_utils.h"

CHECK_NARROWING();

// A bare preset name lives in the config xrcfg folder; anything with a
// path separator or an extension is a DOS file path.
static bool xrcfg_arg_is_path(const std::string& arg)
{
	return arg.find_first_of("\\/:.") != std::string::npos;
}

static std::string xrcfg_resolve_path(const std::string& arg)
{
	const auto slash = arg.find_last_of("\\/");
	const std::string dir = (slash == std::string::npos)
	                              ? std::string()
	                              : arg.substr(0, slash + 1);
	std::string filename  = (slash == std::string::npos)
	                              ? arg
	                              : arg.substr(slash + 1);
	if (filename.empty() || filename == "." || filename == "..") {
		return {};
	}
	for (const char c : filename) {
		if (c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
		    c == '>' || c == '|') {
			return {};
		}
	}
	if (filename.find('.') == std::string::npos) {
		filename += ".xrcfg";
	}
	// Resolve the directory with a short placeholder name so DOS_MakeName
	// cannot truncate the real extension to three characters.
	const std::string probe = dir + "__XRCFG.TMP";
	char fullname[DOS_PATHLENGTH] = {};
	uint8_t drive                 = 0;
	if (!DOS_MakeName(probe.c_str(), fullname, &drive)) {
		return {};
	}
	if (drive >= DOS_DRIVES || !Drives[drive]) {
		return {};
	}
	const auto local_drive = std::dynamic_pointer_cast<localDrive>(
	        Drives[drive]);
	if (!local_drive) {
		return {};
	}
	std::string dos_path = fullname;
	const auto last_sep  = dos_path.find_last_of('\\');
	if (last_sep == std::string::npos) {
		return {};
	}
	dos_path.resize(last_sep + 1);
	dos_path += filename;
	const char* path_part = std::strchr(dos_path.c_str(), ':');
	path_part             = path_part ? path_part + 1 : dos_path.c_str();
	return local_drive->MapDosToHostFilename(path_part);
}

void XRCFG::Run()
{
	if (HelpRequested()) {
		WriteOut("Manages XR presets (the [xr] settings saved under\n"
		         "xr_ key names).\n"
		         "\n"
		         "Usage:\n"
		         "  XRCFG LIST          List saved presets.\n"
		         "  XRCFG SAVE <name>   Save current [xr] settings.\n"
		         "  XRCFG LOAD <name>   Apply a saved preset now.\n"
		         "\n"
		         "<name> is a preset in the config xrcfg folder. A name\n"
		         "with a path separator or extension is a DOS file path,\n"
		         "for example C:\\PRESETS\\GAME.XRCFG.\n");
		return;
	}
	std::string sub;
	if (!cmd->FindCommand(1, sub)) {
		WriteOut("Missing subcommand. Usage: XRCFG LIST, SAVE <name>, LOAD <name>.\n");
		return;
	}
	upcase(sub);
	if (sub == "LIST") {
		const auto names = OPENXR_ListConfigs();
		if (names.empty()) {
			WriteOut("No XR presets saved yet.\n");
			return;
		}
		for (const auto& name : names) {
			WriteOut("  %s\n", name.c_str());
		}
		return;
	}
	std::string name;
	if (!cmd->FindCommand(2, name)) {
		WriteOut("Missing preset name. Usage: XRCFG %s <name>.\n",
		         sub.c_str());
		return;
	}
	const bool path_arg = xrcfg_arg_is_path(name);
	std::string target;
	if (path_arg) {
		target = xrcfg_resolve_path(name);
		if (target.empty()) {
			WriteOut("Invalid XR preset path '%s'.\n", name.c_str());
			return;
		}
	}
	if (sub == "SAVE") {
		const bool saved = path_arg ? OPENXR_SaveConfigToFile(target)
		                            : OPENXR_SaveConfig(name);
		if (saved) {
			WriteOut("Saved XR preset '%s'.\n", name.c_str());
		} else {
			WriteOut("Could not save XR preset '%s'.\n", name.c_str());
		}
		return;
	}
	if (sub == "LOAD") {
		const bool loaded = path_arg ? OPENXR_LoadConfigFromFile(target)
		                             : OPENXR_LoadConfig(name);
		if (loaded) {
			WriteOut("Applied XR preset '%s'.\n", name.c_str());
		} else {
			WriteOut("Could not load XR preset '%s'.\n", name.c_str());
		}
		return;
	}
	WriteOut("Unknown subcommand '%s'. Usage: XRCFG LIST, SAVE <name>, LOAD <name>.\n",
	         sub.c_str());
}
