#pragma once

#include "calendar.h"

/* "dinner with Ana thursday 7pm", "reunión mañana de 10 a 11", "gym every monday 6am".
 * Understands Spanish and English dates, times, durations and repeats with
 * plain rules, so it works offline and instantly; NPU Chat can ask the model
 * for the harder phrasings. Without a time the event lasts all day. */

typedef struct {
  char     *title;
  gint64    start;
  gint64    end;
  gboolean  all_day;
  CalRepeat repeat;
  guint     interval;   /* every N …; 0 for the plain repeat */
  gint64    until;      /* last day of the repeat, 0 for none */
  int       count;      /* number of showings, 0 for none */
  int       alert;      /* minutes before, -1 for none */
  gboolean  reminder;   /* "remind me": a task to tick off */
} CalQuick;

/* now: unix time that "today", "tomorrow" and weekdays are counted from.
 * month_first: "10/11" means October 11th rather than 10 November. */
gboolean calendar_quick_parse (const char *text, gint64 now, gboolean month_first, CalQuick *out);
void     calendar_quick_clear (CalQuick *q);

/* The event a parsed phrase describes, with its repeat, alert and reminder settings. */
CalEvent *calendar_quick_to_event (const CalQuick *q, const char *untitled);
