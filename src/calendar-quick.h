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
} CalQuick;

/* now: unix time that "today", "tomorrow" and weekdays are counted from.
 * month_first: "10/11" means October 11th rather than 10 November. */
gboolean calendar_quick_parse (const char *text, gint64 now, gboolean month_first, CalQuick *out);
void     calendar_quick_clear (CalQuick *q);
