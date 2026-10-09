#pragma once

#include <glib.h>

/* An AppImage is not installed anywhere, so the desktop has no name or icon for it. On start the
 * calendar adds its own menu entry and icon for your user only (under ~/.local/share), and keeps
 * the entry pointing at wherever the AppImage is now. Deleting those two files undoes it.
 * Returns TRUE when something was written. */
gboolean calendar_integrate (const char *appimage, const char *appdir, const char *data_dir);
