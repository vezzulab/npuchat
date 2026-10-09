#pragma once

#include <glib.h>

/* NPU Chat carries its own calendar window. This gives it a "Calendar" entry in the
 * application menu (for your user only, under ~/.local/share) that opens just that
 * window, so the calendar shows up like any other app. Deleting the file undoes it.
 * Returns TRUE when something was written. */
gboolean launcher_install_calendar_entry (const char *appimage, const char *appdir, const char *data_dir);
