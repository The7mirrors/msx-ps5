// MSX PS5 frontend: settings file.
// SPDX-License-Identifier: MIT

#include "fe_settings.h"

#include "OrbisPaths.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace fe
{
namespace
{
std::string IniPath()
{
	return OrbisRoot() + "/msx-ps5.ini";
}

int Clamp(int v, int lo, int hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

// emu::kConsoleButtons' keys, in their order
const char* const kButtonKeys[Settings::kButtons] = {"btn_j1", "btn_j2", "btn_space", "btn_return", "btn_esc", "btn_f1",
	"btn_f2", "btn_f3", "btn_f4", "btn_f5", "btn_m", "btn_n", "btn_graph", "btn_ctrl", "btn_shift", "btn_stop",
	"btn_select", "btn_osk"};
constexpr int kPs5ButtonChoices = 9; // emu::kPs5ButtonCount
constexpr int kShaderCount = 12; // ps5crt::Shader::Count
} // namespace

constexpr int Settings::kDefaultButtons[Settings::kButtons];

void Settings::DefaultButtons()
{
	for (int i = 0; i < kButtons; i++)
		buttons[i] = kDefaultButtons[i];
}

Settings& Config()
{
	static Settings s;
	return s;
}

void Settings::Load()
{
	FILE* f = fopen(IniPath().c_str(), "r");
	if (!f)
		return;
	char line[1024];
	while (fgets(line, sizeof(line), f))
	{
		line[strcspn(line, "\r\n")] = 0;
		char* eq = strchr(line, '=');
		if (!eq || line[0] == '#' || line[0] == ';')
			continue;
		*eq = 0;
		const std::string key = line;
		const char* val = eq + 1;
		const int n = atoi(val);
		if (key == "scale")
			scale = Clamp(n, 0, 2);
		else if (key == "shader")
			shader = Clamp(n, 0, kShaderCount - 1);
		else if (key == "aspect")
			aspect = Clamp(n, 0, 3);
		else if (key == "ntsc")
			ntsc = Clamp(n, 0, 4);
		else if (key == "borders")
			borders = Clamp(n, 0, 3);
		else if (key == "gg_lcd")
			gg_lcd = n != 0;
		else if (key == "smooth")
			smooth = n != 0;
		else if (key == "scanlines")
			scanlines = n != 0;
		else if (key == "show_fps")
			show_fps = n != 0;
		else if (key == "audio")
			audio = n != 0;
		else if (key == "volume")
			volume = Clamp(n, 0, 100);
		else if (key == "fm_chip")
			fm_chip = Clamp(n, 0, 3);
		else if (key == "lowpass")
			lowpass = n != 0;
		else if (key == "ff_speed")
			ff_speed = n == 0 ? 0 : Clamp(n, 150, 1000);
		else if (key == "rewind")
			rewind = n != 0;
		else if (key == "region")
			region = Clamp(n, 0, 4);
		else if (key == "shelf_music")
			shelf_music = Clamp(n, 0, 10);
		else if (key == "shelf_music_volume")
			shelf_music_volume = Clamp(n, 0, 100);
		else if (key == "shelf_sounds")
			shelf_sounds = n != 0;
		else if (key == "no_sprite_limit")
			no_sprite_limit = n != 0;
		else if (key == "pad_type")
			pad_type = Clamp(n, 0, 2);
		else if (key == "multitap")
			multitap = Clamp(n, 0, 2);
		else if (key.compare(0, 4, "btn_") == 0)
		{
			for (int i = 0; i < kButtons; i++)
				if (key == kButtonKeys[i])
					buttons[i] = Clamp(n, 0, kPs5ButtonChoices - 1);
		}
		else if (key == "state_slot")
			state_slot = Clamp(n, 1, 10);
		else if (key == "covers_download")
			covers_download = n != 0;
		else if (key == "debug_logs")
			debug_logs = n != 0;
		else if (key == "shelf_family")
			shelf_family = Clamp(n, 0, 64);
		else if (key == "shelf_theme")
			shelf_theme = Clamp(n, 0, 4);
		else if (key == "shelf_style")
			shelf_style = Clamp(n, 0, 2);
		else if (key == "one_version")
			one_version = n != 0;
		else if (key == "last_rom")
			last_rom = val;
	}
	fclose(f);
	for (int i = 0; i < kButtons; i++)
		all_buttons[i] = buttons[i];
	OrbisLog("[settings] loaded %s", IniPath().c_str());
}

namespace
{
// /data/msx/controls/<the game's file name without its extension>.ini
std::string ControlsPathFor(const std::string& rom)
{
	std::string base = rom.substr(rom.find_last_of('/') + 1);
	const size_t dot = base.find_last_of('.');
	if (dot != std::string::npos && dot > 0)
		base.resize(dot);
	return OrbisRoot() + "/controls/" + base + ".ini";
}
} // namespace

void Settings::BeginGameControls(const std::string& rom)
{
	game_controls = ControlsPathFor(rom);
	for (int i = 0; i < kButtons; i++)
		buttons[i] = all_buttons[i];
	FILE* f = fopen(game_controls.c_str(), "r");
	if (!f)
		return;
	char line[256];
	while (fgets(line, sizeof(line), f))
	{
		line[strcspn(line, "\r\n")] = 0;
		char* eq = strchr(line, '=');
		if (!eq || line[0] == '#')
			continue;
		*eq = 0;
		for (int i = 0; i < kButtons; i++)
			if (!strcmp(line, kButtonKeys[i]))
				buttons[i] = Clamp(atoi(eq + 1), 0, kPs5ButtonChoices - 1);
	}
	fclose(f);
	OrbisLog("[settings] this game's own controls: %s", game_controls.c_str());
}

void Settings::EndGameControls()
{
	game_controls.clear();
	for (int i = 0; i < kButtons; i++)
		buttons[i] = all_buttons[i];
}

void Settings::SaveGameControls() const
{
	if (game_controls.empty())
		return;
	OrbisMkdirs(OrbisRoot() + "/controls");
	FILE* f = fopen(game_controls.c_str(), "w");
	if (!f)
	{
		OrbisLog("[settings] can't write %s", game_controls.c_str());
		return;
	}
	fprintf(f, "# MSX PS5: this game's buttons (Settings > Controls while it runs)\n");
	for (int i = 0; i < kButtons; i++)
		fprintf(f, "%s=%d\n", kButtonKeys[i], buttons[i]);
	fclose(f);
}

void Settings::ForgetGameControls()
{
	if (!game_controls.empty())
		remove(game_controls.c_str());
	for (int i = 0; i < kButtons; i++)
		buttons[i] = all_buttons[i];
}

bool Settings::GameHasControls() const
{
	return !game_controls.empty() && access(game_controls.c_str(), F_OK) == 0;
}

void Settings::Save() const
{
	const std::string tmp = IniPath() + ".tmp";
	FILE* f = fopen(tmp.c_str(), "w");
	if (!f)
	{
		OrbisLog("[settings] can't write %s", tmp.c_str());
		return;
	}
	fprintf(f, "# MSX PS5\n");
	fprintf(f, "scale=%d\n", scale);
	fprintf(f, "shader=%d\n", shader);
	fprintf(f, "aspect=%d\n", aspect);
	fprintf(f, "ntsc=%d\n", ntsc);
	fprintf(f, "borders=%d\n", borders);
	fprintf(f, "gg_lcd=%d\n", gg_lcd ? 1 : 0);
	fprintf(f, "smooth=%d\n", smooth ? 1 : 0);
	fprintf(f, "scanlines=%d\n", scanlines ? 1 : 0);
	fprintf(f, "show_fps=%d\n", show_fps ? 1 : 0);
	fprintf(f, "audio=%d\n", audio ? 1 : 0);
	fprintf(f, "volume=%d\n", volume);
	fprintf(f, "fm_chip=%d\n", fm_chip);
	fprintf(f, "shelf_music=%d\n", shelf_music);
	fprintf(f, "shelf_music_volume=%d\n", shelf_music_volume);
	fprintf(f, "shelf_sounds=%d\n", shelf_sounds ? 1 : 0);
	fprintf(f, "lowpass=%d\n", lowpass ? 1 : 0);
	fprintf(f, "ff_speed=%d\n", ff_speed);
	fprintf(f, "rewind=%d\n", rewind ? 1 : 0);
	fprintf(f, "region=%d\n", region);
	fprintf(f, "no_sprite_limit=%d\n", no_sprite_limit ? 1 : 0);
	fprintf(f, "pad_type=%d\n", pad_type);
	fprintf(f, "multitap=%d\n", multitap);
	for (int i = 0; i < kButtons; i++)
		fprintf(f, "%s=%d\n", kButtonKeys[i], all_buttons[i]); // (a game's own buttons live in its file)
	fprintf(f, "state_slot=%d\n", state_slot);
	fprintf(f, "covers_download=%d\n", covers_download ? 1 : 0);
	fprintf(f, "debug_logs=%d\n", debug_logs ? 1 : 0);
	fprintf(f, "shelf_family=%d\n", shelf_family);
	fprintf(f, "shelf_style=%d\n", shelf_style);
	fprintf(f, "shelf_theme=%d\n", shelf_theme);
	fprintf(f, "one_version=%d\n", one_version ? 1 : 0);
	fprintf(f, "last_rom=%s\n", last_rom.c_str());
	bool ok = ferror(f) == 0;
	ok = fflush(f) == 0 && ok;
	ok = fsync(fileno(f)) == 0 && ok;
	ok = fclose(f) == 0 && ok;
	if (!ok || rename(tmp.c_str(), IniPath().c_str()) != 0)
	{
		remove(tmp.c_str());
		OrbisLog("[settings] can't save %s (disk full?)", IniPath().c_str());
	}
}
} // namespace fe
