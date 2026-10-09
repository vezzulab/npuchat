#pragma once

#include <glib.h>

/* The calendar's settings, in ~/.config/calendar/settings.ini. */
typedef struct {
  int       first_day;       /* 0 follows the language; 1 Monday … 7 Sunday */
  int       days;            /* days shown per week: 5 (Monday to Friday) or 7 */
  int       day_start;       /* the working day, in hours; the views start there */
  int       day_end;
  gboolean  week_numbers;
  int       default_alert;   /* minutes for new events, -1 for none */
  char     *default_calendar;/* id, or NULL for the first of the user's own */
  char      theme[16];       /* "system", "light" or "dark" (the standalone app) */
  gboolean  background;      /* keep running for alerts after the window closes */
  gboolean  autostart;       /* start in the background when you log in */
} CalSettings;

CalSettings *calendar_settings (void);
void         calendar_settings_save (void);
void         calendar_settings_free (void);
