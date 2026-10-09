#include "calendar-settings.h"

#include <glib/gstdio.h>
#include <string.h>

static CalSettings *settings;

static char *
path (void)
{
  return g_build_filename (g_get_user_config_dir (), "calendar", "settings.ini", NULL);
}

static int
get_int (GKeyFile *kf, const char *key, int fallback, int lo, int hi)
{
  g_autoptr (GError) error = NULL;
  int v = g_key_file_get_integer (kf, "calendar", key, &error);
  return error ? fallback : CLAMP (v, lo, hi);
}

static gboolean
get_bool (GKeyFile *kf, const char *key, gboolean fallback)
{
  g_autoptr (GError) error = NULL;
  gboolean v = g_key_file_get_boolean (kf, "calendar", key, &error);
  return error ? fallback : v;
}

CalSettings *
calendar_settings (void)
{
  if (settings)
    return settings;
  settings = g_new0 (CalSettings, 1);
  g_autoptr (GKeyFile) kf = g_key_file_new ();
  g_autofree char *p = path ();
  g_key_file_load_from_file (kf, p, G_KEY_FILE_NONE, NULL);
  settings->first_day = get_int (kf, "first_day", 0, 0, 7);
  settings->days = get_int (kf, "days", 7, 5, 7) < 7 ? 5 : 7;
  settings->day_start = get_int (kf, "day_start", 8, 0, 22);
  settings->day_end = get_int (kf, "day_end", 18, settings->day_start + 1, 24);
  settings->week_numbers = get_bool (kf, "week_numbers", FALSE);
  settings->default_alert = get_int (kf, "default_alert", -1, -1, 10080);
  settings->background = get_bool (kf, "background", FALSE);
  settings->autostart = get_bool (kf, "autostart", FALSE);
  g_autofree char *cal = g_key_file_get_string (kf, "calendar", "default_calendar", NULL);
  settings->default_calendar = cal && *cal ? g_strdup (cal) : NULL;
  g_autofree char *theme = g_key_file_get_string (kf, "calendar", "theme", NULL);
  g_strlcpy (settings->theme, theme && (g_str_equal (theme, "light") || g_str_equal (theme, "dark")) ? theme : "system",
             sizeof settings->theme);
  return settings;
}

void
calendar_settings_save (void)
{
  CalSettings *s = calendar_settings ();
  g_autoptr (GKeyFile) kf = g_key_file_new ();
  g_key_file_set_integer (kf, "calendar", "first_day", s->first_day);
  g_key_file_set_integer (kf, "calendar", "days", s->days);
  g_key_file_set_integer (kf, "calendar", "day_start", s->day_start);
  g_key_file_set_integer (kf, "calendar", "day_end", s->day_end);
  g_key_file_set_boolean (kf, "calendar", "week_numbers", s->week_numbers);
  g_key_file_set_integer (kf, "calendar", "default_alert", s->default_alert);
  g_key_file_set_boolean (kf, "calendar", "background", s->background);
  g_key_file_set_boolean (kf, "calendar", "autostart", s->autostart);
  g_key_file_set_string (kf, "calendar", "default_calendar", s->default_calendar ? s->default_calendar : "");
  g_key_file_set_string (kf, "calendar", "theme", s->theme);
  g_autofree char *p = path ();
  g_autofree char *dir = g_path_get_dirname (p);
  g_mkdir_with_parents (dir, 0700);
  g_key_file_save_to_file (kf, p, NULL);
}

void
calendar_settings_free (void)
{
  if (!settings)
    return;
  g_free (settings->default_calendar);
  g_free (settings);
  settings = NULL;
}
