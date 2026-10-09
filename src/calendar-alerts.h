#pragma once

#include <gio/gio.h>

/* Notifications shown before events that have an alert. One timer is armed for
 * the next alert only (never polling), and it re-arms after an alert fires, when
 * events change, and at least once an hour so a suspended laptop catches up.
 * Each notification offers "Snooze", which brings it back later, and opens the
 * app when clicked. */

void calendar_alerts_start (GApplication *app);
void calendar_alerts_reschedule (void);
void calendar_alerts_stop (void);

/* Shows what is due at `now` (notifications go out only when the app is started) and
 * returns how many alerts that was. Exposed for tests; the timer calls it. */
guint calendar_alerts_poll (gint64 now);
/* Brings an alert for this showing back in `seconds`. */
void  calendar_alerts_snooze (const char *event_id, gint64 occurrence_start, int seconds);
/* The moment the timer should wake next, never more than an hour away. */
gint64 calendar_alerts_next_wake (gint64 now);

#define CALENDAR_SNOOZE_SECONDS (9 * 60)
