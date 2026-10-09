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

/* How a monthly event picks its day. */
typedef enum {
  CAL_MONTHLY_DATE,     /* the same day number: the 15th */
  CAL_MONTHLY_WEEKDAY,  /* the same weekday: the second Tuesday, or the last one */
  CAL_MONTHLY_LAST_DAY, /* the last day of the month */
} CalMonthly;

#define CAL_N_COLORS 8

/* A calendar groups events and gives them a color ("Personal", "Work"…). */
typedef struct {
  char     *id;
  char     *name;
  guint     color;         /* 0 .. CAL_N_COLORS-1, see calendar_color_hex */
  gboolean  visible;
  /* subscriptions: read-only calendars fed from a web address */
  char     *url;           /* NULL for the user's own calendars */
  int       refresh_hours; /* how often to fetch it again */
  gint64    fetched;       /* unix time of the last successful fetch */
  /* a calendar kept in step with a CalDAV server (iCloud, Nextcloud…) */
  char     *account;       /* id of the account, NULL when purely local */
  char     *href;          /* the calendar's address on the server */
  char     *sync_state;    /* the server's tag for what we last saw (CTag) */
} CalCalendar;

typedef struct {
  char     *id;
  char     *uid;           /* the iCalendar UID; every event gets one when added */
  char     *href;          /* where it lives on the CalDAV server, NULL if it was never uploaded */
  char     *etag;          /* the server's tag for the version we have */
  gboolean  dirty;         /* changed here since the last sync */
  gint64    recurrence_id; /* for a changed single showing of a repeating event: which one (else 0) */
  char     *calendar;      /* id of its CalCalendar */
  char     *title;
  char     *notes;
  char     *location;
  char     *url;           /* a link kept with the event */
  GArray   *alerts;        /* int: minutes before the start (negative: after it); empty for none */
  gint64    start;         /* unix seconds; an all-day event starts at local midnight */
  gint64    end;           /* exclusive; always after start; may be days later */
  gboolean  all_day;
  gboolean  reminder;      /* a task with a check box instead of a block of time */
  gboolean  done;          /* a completed reminder that does not repeat */
  GArray   *completed;     /* gint64: finished showings of a repeating reminder */
  CalRepeat repeat;
  guint     interval;      /* every N days/weeks/months/years; 0 means 1 */
  guint     weekdays;      /* weekly: bit 0 Monday … bit 6 Sunday; 0 means the start's weekday */
  CalMonthly monthly;
  int       count;         /* the series ends after this many showings; 0 means never */
  gint64    until;         /* last day a repeating event may start on, 0 = forever */
  GArray   *exceptions;    /* gint64: showings removed from a repeating event ('only this one' edits) */
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
/* alerts */
void      calendar_event_set_alerts (CalEvent *ev, const int *minutes, guint n);
void      calendar_event_add_alert (CalEvent *ev, int minutes);
int       calendar_event_alert (const CalEvent *ev, guint index);   /* -1 past the end */
guint     calendar_event_alert_count (const CalEvent *ev);

/* ---- calendars ---- */
const char *calendar_color_hex (guint color);
/* The index of our colour closest to "#RRGGBB", for calendars that come with their own. */
guint calendar_color_nearest (const char *hex);
GPtrArray  *calendar_calendars (Calendar *cal);              /* CalCalendar*, never empty */
CalCalendar *calendar_calendar_find (Calendar *cal, const char *id);  /* falls back to the first */
const char *calendar_calendar_add (Calendar *cal, const char *name, guint color);
/* A calendar fed from a web address; its events are replaced by calendar_replace_events. */
const char *calendar_subscription_add (Calendar *cal, const char *name, guint color, const char *url, int refresh_hours);
void        calendar_calendar_changed (Calendar *cal);       /* after editing a CalCalendar in place */
/* Deletes the calendar and its events; the last calendar cannot be removed. */
gboolean    calendar_calendar_remove (Calendar *cal, const char *id);
/* Swaps all the events of a calendar for these (takes ownership of the array's events). */
void        calendar_replace_events (Calendar *cal, const char *calendar_id, GPtrArray *events);
/* ---- keeping a calendar in step with a CalDAV server ---- */
typedef struct {
  char *calendar;  /* local calendar id */
  char *href;
  char *etag;
} CalPendingDelete;

/* A local calendar linked to a server calendar. */
const char *calendar_linked_add (Calendar *cal, const char *name, guint color, const char *account, const char *href);
/* Called after every save; the sync uses it to push changes soon. */
void calendar_set_change_hook (void (*hook) (void));
/* Events of a calendar that changed here (const CalEvent*). Free the array with g_ptr_array_unref. */
GPtrArray *calendar_dirty_events (Calendar *cal, const char *calendar_id);
/* All events that belong to one resource: the series and its changed showings. */
GPtrArray *calendar_events_with_uid (Calendar *cal, const char *calendar_id, const char *uid);
/* After an upload: the event now lives at href with this tag, and is clean. */
void calendar_mark_synced (Calendar *cal, const char *calendar_id, const char *uid, const char *href, const char *etag);
/* From the server: these events (all with the same href and tag) replace what we have at that href. */
void calendar_replace_resource (Calendar *cal, const char *calendar_id, const char *href, GPtrArray *events);
/* The server no longer has this resource. */
void calendar_remove_resource (Calendar *cal, const char *calendar_id, const char *href);
const GPtrArray *calendar_pending_deletes (Calendar *cal);           /* CalPendingDelete* */
void calendar_clear_pending_delete (Calendar *cal, const char *href);
/* Every calendar linked to an account, and a way to drop an account's calendars. */
void calendar_unlink_account (Calendar *cal, const char *account, gboolean delete_events);

/* The first calendar that accepts new events (not a subscription). */
const CalCalendar *calendar_default_target (Calendar *cal);

/* Adds all these events to a calendar and saves once. Takes ownership of the events and empties
 * the array. Returns how many were added. */
guint calendar_import (Calendar *cal, GPtrArray *events, const char *calendar_id);

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
/* Every stored event, in the order they were added (CalEvent*). Do not modify the array. */
const GPtrArray *calendar_all_events (Calendar *cal);

/* Deleted events wait here, so a deletion (the user's or the model's) can be undone. */
gboolean        calendar_undo_delete (Calendar *cal, char **title);  /* brings back the last one */

/* Marks one showing of a reminder done or not done. */
void            calendar_set_done (Calendar *cal, const char *id, gint64 occ_start, gboolean done);
gboolean        calendar_is_done (const CalEvent *ev, gint64 occ_start);

/* Occurrences that overlap [from, to), sorted by start. Free with g_array_free (arr, TRUE). */
GArray *calendar_occurrences (Calendar *cal, gint64 from, gint64 to);

/* Gaps of at least `minutes` inside [from, to) between the visible events, restricted
 * to the hours [hour_from, hour_to) of each day. Each pair is start, end (gint64). */
GArray *calendar_free_slots (Calendar *cal, gint64 from, gint64 to, int minutes, int hour_from, int hour_to);

/* The next alert that fires after the given time: when, and for which showing of
 * which event. Looks about ten days ahead. FALSE when nothing is scheduled. */
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
