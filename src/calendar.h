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

#define CAL_NO_ALERT (-1)
#define CAL_N_COLORS 8

/* A calendar groups events and gives them a color ("Personal", "Work"…). */
typedef struct {
  char     *id;
  char     *name;
  guint     color;    /* 0 .. CAL_N_COLORS-1, see calendar_color_hex */
  gboolean  visible;
} CalCalendar;

typedef struct {
  char     *id;
  char     *calendar;  /* id of its CalCalendar */
  char     *title;
  char     *notes;
  char     *location;
  int       alert;     /* minutes before the start, CAL_NO_ALERT for none */
  gint64    start;    /* unix seconds; an all-day event starts at local midnight */
  gint64    end;      /* exclusive; always after start */
  gboolean  all_day;
  CalRepeat repeat;
  gint64    until;    /* last day a repeating event may start on, 0 = forever */
  GArray   *exceptions; /* gint64: showings removed from a repeating event ('only this one' edits) */
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

/* ---- calendars ---- */
const char *calendar_color_hex (guint color);
GPtrArray  *calendar_calendars (Calendar *cal);              /* CalCalendar*, never empty */
CalCalendar *calendar_calendar_find (Calendar *cal, const char *id);  /* falls back to the first */
const char *calendar_calendar_add (Calendar *cal, const char *name, guint color);
void        calendar_calendar_changed (Calendar *cal);       /* after editing a CalCalendar in place */
/* Deletes the calendar and its events; the last calendar cannot be removed. */
gboolean    calendar_calendar_remove (Calendar *cal, const char *id);

/* Takes ownership of ev, gives it an id and saves. Returns the id (owned by the calendar). */
const char     *calendar_add (Calendar *cal, CalEvent *ev);
/* Editing or deleting one showing of a repeating event. */
typedef enum { CAL_SCOPE_ONE, CAL_SCOPE_FUTURE, CAL_SCOPE_ALL } CalScope;

/* occ_start is the start of the showing the user acted on. For an event that does not
 * repeat the scope makes no difference. */
gboolean calendar_apply_delete (Calendar *cal, const char *id, gint64 occ_start, CalScope scope);
/* edited carries the new values; its start and end are the new time of that showing.
 * Takes ownership. */
gboolean calendar_apply_edit (Calendar *cal, const char *id, gint64 occ_start, CalScope scope, CalEvent *edited);

/* Replaces the stored event that has the same id as ev; takes ownership. */
gboolean        calendar_update (Calendar *cal, CalEvent *ev);
gboolean        calendar_remove (Calendar *cal, const char *id);
const CalEvent *calendar_find (Calendar *cal, const char *id);
guint           calendar_count (Calendar *cal);

/* Occurrences that overlap [from, to), sorted by start. Free with g_array_free (arr, TRUE). */
GArray *calendar_occurrences (Calendar *cal, gint64 from, gint64 to);

/* The next alert that fires after the given time: when, and for which showing of
 * which event. Looks about nine days ahead. FALSE when nothing is scheduled. */
typedef struct {
  const CalEvent *event;
  gint64          fire;
  gint64          start;
} CalAlert;
gboolean calendar_next_alert (Calendar *cal, gint64 after, CalAlert *out);

/* Midnight (local) of the day that contains t, and the day after. */
gint64 calendar_day_start (gint64 t);
gint64 calendar_day_next (gint64 t);

/* The calendar the app uses. */
Calendar *calendar_default (void);
void      calendar_default_free (void);

/* For the model: "2026-10-09" or "2026-10-09T15:30", local time. */
gboolean calendar_parse_time (const char *text, gint64 *t, gboolean *date_only);
