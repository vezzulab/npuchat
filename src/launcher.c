#include "launcher.h"

#include <glib/gstdio.h>
#include <string.h>

#define ENTRY_ID "io.github.vezzulab.NpuChat.Calendar"

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
launcher_install_calendar_entry (const char *appimage, const char *appdir, const char *data_dir)
{
  if (!data_dir || !*data_dir)
    return FALSE;

  /* an AppImage runs from where it says it is; otherwise, this very program */
  g_autofree char *self = NULL;
  if (appimage && *appimage && g_file_test (appimage, G_FILE_TEST_IS_REGULAR))
    self = g_strdup (appimage);
  else
    self = g_file_read_link ("/proc/self/exe", NULL);
  if (!self || !g_path_is_absolute (self))
    return FALSE;

  g_autofree char *quoted = g_shell_quote (self);
  g_autofree char *desktop = g_strdup_printf (
    "[Desktop Entry]\n"
    "Type=Application\n"
    "Name=Calendar\n"
    "Name[es]=Calendario\n"
    "Comment=A simple, private calendar\n"
    "Comment[es]=Un calendario simple y privado\n"
    "Exec=%s --calendar\n"
    "Icon=" ENTRY_ID "\n"
    "Terminal=false\n"
    "Categories=Office;Calendar;GTK;\n"
    "Keywords=calendar;calendario;events;agenda;\n"
    "StartupNotify=false\n", quoted);
  g_autofree char *desktop_path = g_build_filename (data_dir, "applications", ENTRY_ID ".desktop", NULL);
  gboolean changed = write_if_different (desktop_path, desktop);

  /* the icon travels inside the AppImage */
  if (appdir && *appdir)
    {
      g_autofree char *from = g_build_filename (appdir, "usr", "share", "icons", "hicolor", "scalable", "apps", ENTRY_ID ".svg", NULL);
      g_autofree char *icon = NULL;
      if (g_file_get_contents (from, &icon, NULL, NULL))
        {
          g_autofree char *to = g_build_filename (data_dir, "icons", "hicolor", "scalable", "apps", ENTRY_ID ".svg", NULL);
          changed |= write_if_different (to, icon);
        }
    }
  return changed;
}
