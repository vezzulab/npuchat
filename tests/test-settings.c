#include <glib.h>
#include <glib/gstdio.h>

#include "../src/calendar-settings.h"

static void
test_settings (void)
{
  g_autofree char *tmp = g_dir_make_tmp ("settings-XXXXXX", NULL);
  g_setenv ("XDG_CONFIG_HOME", tmp, TRUE);
  CalSettings *s = calendar_settings ();
  /* defaults */
  g_assert_cmpint (s->first_day, ==, 0);
  g_assert_cmpint (s->days, ==, 7);
  g_assert_cmpint (s->day_start, ==, 8);
  g_assert_cmpint (s->day_end, ==, 18);
  g_assert_false (s->week_numbers);
  g_assert_false (s->background_asked);
  g_assert_cmpint (s->default_alert, ==, -1);
  g_assert_cmpstr (s->theme, ==, "system");
  g_assert_null (s->default_calendar);

  s->first_day = 7;
  s->days = 5;
  s->day_start = 6;
  s->day_end = 22;
  s->week_numbers = TRUE;
  s->default_alert = 15;
  s->background = TRUE;
  s->background_asked = TRUE;
  s->default_calendar = g_strdup ("abc");
  g_strlcpy (s->theme, "dark", sizeof s->theme);
  calendar_settings_save ();
  calendar_settings_free ();

  s = calendar_settings ();
  g_assert_cmpint (s->first_day, ==, 7);
  g_assert_cmpint (s->days, ==, 5);
  g_assert_cmpint (s->day_start, ==, 6);
  g_assert_cmpint (s->day_end, ==, 22);
  g_assert_true (s->week_numbers);
  g_assert_cmpint (s->default_alert, ==, 15);
  g_assert_true (s->background);
  g_assert_true (s->background_asked);
  g_assert_cmpstr (s->default_calendar, ==, "abc");
  g_assert_cmpstr (s->theme, ==, "dark");

  /* nonsense in the file is clamped instead of trusted */
  g_autofree char *p = g_build_filename (tmp, "calendar", "settings.ini", NULL);
  g_file_set_contents (p, "[calendar]\nfirst_day=99\ndays=3\nday_start=30\nday_end=2\ntheme=pink\ndefault_alert=-50\n", -1, NULL);
  calendar_settings_free ();
  s = calendar_settings ();
  g_assert_cmpint (s->first_day, ==, 7);
  g_assert_cmpint (s->days, ==, 5);
  g_assert_cmpint (s->day_start, ==, 22);
  g_assert_cmpint (s->day_end, >, s->day_start);
  g_assert_cmpstr (s->theme, ==, "system");
  g_assert_cmpint (s->default_alert, ==, -1);
  calendar_settings_free ();
  g_remove (p);
  g_autofree char *dir = g_build_filename (tmp, "calendar", NULL);
  g_rmdir (dir);
  g_rmdir (tmp);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/settings/roundtrip", test_settings);
  return g_test_run ();
}
