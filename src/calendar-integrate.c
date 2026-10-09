#include "calendar-integrate.h"

#include <glib/gstdio.h>
#include <string.h>

#define APP_ID "io.github.vezzulab.Calendar"

static gboolean
write_if_different (const char *path, const char *text)
{
  g_autofree char *old = NULL;
  if (g_file_get_contents (path, &old, NULL, NULL) && g_str_equal (old, text))
    return FALSE;
  g_autofree char *dir = g_path_get_dirname (path);
  g_mkdir_with_parents (dir, 0755);
  return g_file_set_contents (path, text, -1, NULL);
}

gboolean
calendar_integrate (const char *appimage, const char *appdir, const char *data_dir)
{
  /* only for an AppImage that is really where it says it is */
  if (!appimage || !*appimage || !data_dir || !g_file_test (appimage, G_FILE_TEST_IS_REGULAR))
    return FALSE;
  gboolean changed = FALSE;

  g_autofree char *quoted = g_shell_quote (appimage);
  g_autofree char *desktop = g_strdup_printf (
    "[Desktop Entry]\n"
    "Type=Application\n"
    "Name=Calendar\n"
    "Name[es]=Calendario\n"
    "Comment=A simple, private calendar\n"
    "Comment[es]=Un calendario simple y privado\n"
    "Exec=%s\n"
    "Icon=" APP_ID "\n"
    "Terminal=false\n"
    "Categories=Office;Calendar;GTK;\n"
    "Keywords=calendar;calendario;events;agenda;\n"
    "StartupNotify=true\n"
    "StartupWMClass=" APP_ID "\n", quoted);
  g_autofree char *desktop_path = g_build_filename (data_dir, "applications", APP_ID ".desktop", NULL);
  changed |= write_if_different (desktop_path, desktop);

  /* the icon travels inside the AppImage */
  if (appdir && *appdir)
    {
      g_autofree char *from = g_build_filename (appdir, "usr", "share", "icons", "hicolor", "scalable", "apps", APP_ID ".svg", NULL);
      g_autofree char *icon = NULL;
      if (g_file_get_contents (from, &icon, NULL, NULL))
        {
          g_autofree char *to = g_build_filename (data_dir, "icons", "hicolor", "scalable", "apps", APP_ID ".svg", NULL);
          changed |= write_if_different (to, icon);
        }
    }
  return changed;
}
