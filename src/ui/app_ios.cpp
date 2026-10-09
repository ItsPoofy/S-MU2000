// license:BSD-3-Clause
//
// The iOS front end's app globals. The class is ui/app_ios.h. The Mac and
// Windows twins are ui/app_mac.cpp and ui/app_win.cpp, and this file is to them
// what those are to each other: the small platform-specific piece that main()
// and the window file share, and nothing else.
//
// The Mac version of this file is 26 lines and holds three things. Two of them
// exist here for the same reason and the third does not, yet.

#include "app_ios.h"

#include "compat/paths.h"

namespace ui {

gui_app *g_gui = nullptr;

// A MIDI file dropped on the panel. There is no drag and drop on iOS yet - it
// needs a UIDropInteraction or a document picker - so this is the seam the
// window calls when one turns up, and it does nothing until something calls it.
void play_dropped_file(const std::string &path)
{
	if (g_gui)
		g_gui->file_dropped(path);
}

// gui.ini, in the app's own Documents directory.
//
// The Mac side uses ensure_config_dir(), which resolves to
// ~/Library/Application Support/... and is a macOS-shaped path. On iOS the
// writable place is the sandbox container and NSDocumentDirectory is the only
// correct answer to that; hardcoding anything under HOME fails, because HOME is
// not writable in the sandbox.
//
// The std::string{} return is the deliberate bit: this is a .cpp, not a .mm, so
// it cannot ask Foundation for the path. Returning empty means "no settings file
//", which the shared code already handles - it falls back to defaults and simply
// does not remember between launches. Step 1 has no settings to remember anyway.
// When this becomes a .mm, this becomes
//     NSDocumentDirectory + "/S-MU2000/gui.ini"
// with the directory created first, as ensure_config_dir does on the Mac.
std::string settings_file_path()
{
	return {};
}

} // namespace ui