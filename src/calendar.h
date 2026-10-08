#pragma once

#include <glib.h>

/* The calendar's events, kept in one JSON file under the user's data
 * directory. Everything is local; nothing here touches the network. */

typedef enum {
  CAL_REPEAT_NONE,
  CAL_REPEAT_DAILY,
  CAL_REPEAT_WEEKLY,
  CAL_REPEAT_MONTHLY,
  CAL_REPEAT_YEARLY,
} CalRepeat;

typedef struct {
  char     *id;
  char     *title;
  char     *notes;
  gint64    start;    /* unix seconds; an all-day event starts at local midnight */
  gint64    end;      /* exclusive; always after start */
  gboolean  all_day;
  CalRepeat repeat;
  gint64    until;    /* last day a repeating event may start on, 0 = forever */
} CalEvent;

/* One concrete showing of an event inside a time range. */
typedef struct {
  const CalEvent *event;
  gint64          start;
  gint64          end;
} CalOccurrence;

typedef struct _Calendar Calendar;

Calendar *calendar_new (const char *path);   /* loads the file when it exists */
void      calendar_free (Calendar *cal);

CalEvent *calendar_event_new (const char *title, gint64 start, gint64 end, gboolean all_day);
void      calendar_event_free (CalEvent *ev);
CalEvent *calendar_event_copy (const CalEvent *ev);

/* Takes ownership of ev, gives it an id and saves. Returns the id (owned by the calendar). */
const char     *calendar_add (Calendar *cal, CalEvent *ev);
/* Replaces the stored event that has the same id as ev; takes ownership. */
gboolean        calendar_update (Calendar *cal, CalEvent *ev);
gboolean        calendar_remove (Calendar *cal, const char *id);
const CalEvent *calendar_find (Calendar *cal, const char *id);
guint           calendar_count (Calendar *cal);

/* Occurrences that overlap [from, to), sorted by start. Free with g_array_free (arr, TRUE). */
GArray *calendar_occurrences (Calendar *cal, gint64 from, gint64 to);

/* Midnight (local) of the day that contains t, and the day after. */
gint64 calendar_day_start (gint64 t);
gint64 calendar_day_next (gint64 t);

/* The calendar the app uses. */
Calendar *calendar_default (void);
void      calendar_default_free (void);

/* For the model: "2026-10-09" or "2026-10-09T15:30", local time. */
gboolean calendar_parse_time (const char *text, gint64 *t, gboolean *date_only);
