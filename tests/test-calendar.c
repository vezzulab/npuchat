#include <glib.h>
#include <glib/gstdio.h>

#include "../src/calendar.h"

static gint64
at (int y, int mo, int d, int h, int mi)
{
  g_autoptr (GDateTime) dt = g_date_time_new_local (y, mo, d, h, mi, 0);
  return g_date_time_to_unix (dt);
}

static guint
count_in (Calendar *cal, gint64 from, gint64 to)
{
  GArray *a = calendar_occurrences (cal, from, to);
  guint n = a->len;
  g_array_free (a, TRUE);
  return n;
}

static void
test_single (void)
{
  Calendar *cal = calendar_new (NULL);
  calendar_add (cal, calendar_event_new ("Dentist", at (2026, 10, 9, 15, 0), at (2026, 10, 9, 16, 0), FALSE));
  g_assert_cmpuint (count_in (cal, at (2026, 10, 9, 0, 0), at (2026, 10, 10, 0, 0)), ==, 1);
  g_assert_cmpuint (count_in (cal, at (2026, 10, 10, 0, 0), at (2026, 10, 11, 0, 0)), ==, 0);
  /* an event that has already started but not ended still shows */
  g_assert_cmpuint (count_in (cal, at (2026, 10, 9, 15, 30), at (2026, 10, 9, 17, 0)), ==, 1);
  /* the end is exclusive */
  g_assert_cmpuint (count_in (cal, at (2026, 10, 9, 16, 0), at (2026, 10, 9, 17, 0)), ==, 0);
  calendar_free (cal);
}

static void
test_repeat (void)
{
  Calendar *cal = calendar_new (NULL);
  CalEvent *w = calendar_event_new ("Gym", at (2026, 1, 5, 7, 0), at (2026, 1, 5, 8, 0), FALSE);
  w->repeat = CAL_REPEAT_WEEKLY;
  calendar_add (cal, w);
  /* Mondays in October 2026: 5, 12, 19, 26 */
  g_assert_cmpuint (count_in (cal, at (2026, 10, 1, 0, 0), at (2026, 11, 1, 0, 0)), ==, 4);

  CalEvent *m = calendar_event_new ("Rent", at (2026, 1, 31, 9, 0), at (2026, 1, 31, 10, 0), FALSE);
  m->repeat = CAL_REPEAT_MONTHLY;
  calendar_add (cal, m);
  /* the 31st does not drift: February has no 31st, March has */
  g_assert_cmpuint (count_in (cal, at (2026, 3, 31, 0, 0), at (2026, 4, 1, 0, 0)), ==, 1);

  CalEvent *y = calendar_event_new ("Birthday", at (2000, 10, 9, 0, 0), 0, TRUE);
  y->repeat = CAL_REPEAT_YEARLY;
  calendar_add (cal, y);
  g_assert_cmpuint (count_in (cal, at (2026, 10, 9, 0, 0), at (2026, 10, 10, 0, 0)), ==, 1);

  CalEvent *d = calendar_event_new ("Pills", at (2026, 10, 1, 8, 0), at (2026, 10, 1, 8, 5), FALSE);
  d->repeat = CAL_REPEAT_DAILY;
  d->until = at (2026, 10, 3, 0, 0);
  calendar_add (cal, d);
  /* Oct 1-9: Pills on the 1st, 2nd and 3rd, Gym on the 5th, Birthday on the 9th */
  g_assert_cmpuint (count_in (cal, at (2026, 10, 1, 0, 0), at (2026, 10, 10, 0, 0)), ==, 5);
  calendar_free (cal);
}

/* Runaway check: huge ranges and far-away events finish quickly and stay bounded. */
static void
test_bounded (void)
{
  Calendar *cal = calendar_new (NULL);
  CalEvent *d = calendar_event_new ("Daily", at (2000, 1, 1, 8, 0), at (2000, 1, 1, 9, 0), FALSE);
  d->repeat = CAL_REPEAT_DAILY;
  calendar_add (cal, d);
  CalEvent *m = calendar_event_new ("Monthly", at (1971, 1, 1, 8, 0), at (1971, 1, 1, 9, 0), FALSE);
  m->repeat = CAL_REPEAT_MONTHLY;
  calendar_add (cal, m);
  calendar_add (cal, calendar_event_new ("Far", at (2400, 1, 1, 8, 0), 0, FALSE));
  gint64 t0 = g_get_monotonic_time ();
  /* 2000-2029: 10958 days (8 leap years) plus 360 first-of-the-month events */
  g_assert_cmpuint (count_in (cal, at (2000, 1, 1, 0, 0), at (2030, 1, 1, 0, 0)), ==, 10958 + 360);
  /* a one-day window far from the start must not walk every repeat since 2000 */
  g_assert_cmpuint (count_in (cal, at (2026, 10, 9, 0, 0), at (2026, 10, 10, 0, 0)), ==, 1);
  g_assert_cmpint (g_get_monotonic_time () - t0, <, 2 * G_USEC_PER_SEC);
  calendar_free (cal);
}

static void
test_sorted_and_roundtrip (void)
{
  g_autofree char *dir = g_dir_make_tmp ("cal-XXXXXX", NULL);
  g_autofree char *path = g_build_filename (dir, "sub", "calendar.json", NULL);
  Calendar *cal = calendar_new (path);
  calendar_add (cal, calendar_event_new ("B", at (2026, 10, 9, 18, 0), 0, FALSE));
  CalEvent *a = calendar_event_new ("A", at (2026, 10, 9, 9, 0), 0, FALSE);
  g_free (a->notes);
  a->notes = g_strdup ("bring \"documents\"\nand coffee");
  a->repeat = CAL_REPEAT_WEEKLY;
  calendar_add (cal, a);
  calendar_free (cal);

  cal = calendar_new (path);
  g_assert_cmpuint (calendar_count (cal), ==, 2);
  GArray *occ = calendar_occurrences (cal, at (2026, 10, 9, 0, 0), at (2026, 10, 10, 0, 0));
  g_assert_cmpuint (occ->len, ==, 2);
  const CalOccurrence *first = &g_array_index (occ, CalOccurrence, 0);
  g_assert_cmpstr (first->event->title, ==, "A");
  g_assert_cmpstr (first->event->notes, ==, "bring \"documents\"\nand coffee");
  g_assert_cmpint (first->event->repeat, ==, CAL_REPEAT_WEEKLY);
  g_assert_cmpint (first->end - first->start, ==, 3600);
  g_array_free (occ, TRUE);
  calendar_free (cal);
  g_remove (path);
  g_autofree char *sub = g_build_filename (dir, "sub", NULL);
  g_rmdir (sub);
  g_rmdir (dir);
}

static void
test_edit (void)
{
  Calendar *cal = calendar_new (NULL);
  const char *id = calendar_add (cal, calendar_event_new ("Old", at (2026, 10, 9, 9, 0), 0, FALSE));
  g_autofree char *copy_id = g_strdup (id);
  CalEvent *ev = calendar_event_copy (calendar_find (cal, copy_id));
  g_free (ev->title);
  ev->title = g_strdup ("New");
  g_assert_true (calendar_update (cal, ev));
  g_assert_cmpstr (calendar_find (cal, copy_id)->title, ==, "New");
  g_assert_true (calendar_remove (cal, copy_id));
  g_assert_false (calendar_remove (cal, copy_id));
  g_assert_cmpuint (calendar_count (cal), ==, 0);
  /* updating something that is gone fails and frees the event */
  CalEvent *ghost = calendar_event_new ("Ghost", 1000, 0, FALSE);
  ghost->id = g_strdup ("nope");
  g_assert_false (calendar_update (cal, ghost));
  calendar_free (cal);
}

static void
test_calendars (void)
{
  g_autofree char *dir = g_dir_make_tmp ("cal-XXXXXX", NULL);
  g_autofree char *path = g_build_filename (dir, "calendar.json", NULL);
  Calendar *cal = calendar_new (path);
  /* a new calendar starts with one, so events always have somewhere to live */
  g_assert_cmpuint (calendar_calendars (cal)->len, ==, 1);
  g_autofree char *work = g_strdup (calendar_calendar_add (cal, "Work", 2));
  CalEvent *a = calendar_event_new ("Standup", at (2026, 10, 9, 9, 0), 0, FALSE);
  a->calendar = g_strdup (work);
  calendar_event_add_alert (a, 10);
  g_free (a->location);
  a->location = g_strdup ("Room 4");
  calendar_add (cal, a);
  calendar_add (cal, calendar_event_new ("Gym", at (2026, 10, 9, 18, 0), 0, FALSE));
  CalCalendar *w = calendar_calendar_find (cal, work);
  w->visible = FALSE;
  calendar_calendar_changed (cal);
  calendar_free (cal);

  cal = calendar_new (path);
  g_assert_cmpuint (calendar_calendars (cal)->len, ==, 2);
  CalCalendar *w2 = calendar_calendar_find (cal, work);
  g_assert_cmpstr (w2->name, ==, "Work");
  g_assert_cmpuint (w2->color, ==, 2);
  g_assert_false (w2->visible);
  GArray *occ = calendar_occurrences (cal, at (2026, 10, 9, 0, 0), at (2026, 10, 10, 0, 0));
  g_assert_cmpuint (occ->len, ==, 2);
  const CalEvent *standup = g_array_index (occ, CalOccurrence, 0).event;
  g_assert_cmpstr (standup->calendar, ==, work);
  g_assert_cmpint (calendar_event_alert (standup, 0), ==, 10);
  g_assert_cmpstr (standup->location, ==, "Room 4");
  /* the other event fell into the first calendar and has no alert */
  g_assert_cmpuint (calendar_event_alert_count (g_array_index (occ, CalOccurrence, 1).event), ==, 0);
  g_array_free (occ, TRUE);

  /* removing a calendar removes its events; the last one cannot go */
  g_assert_true (calendar_calendar_remove (cal, work));
  g_assert_cmpuint (calendar_count (cal), ==, 1);
  g_assert_false (calendar_calendar_remove (cal, ((CalCalendar *) calendar_calendars (cal)->pdata[0])->id));
  g_assert_cmpuint (calendar_calendars (cal)->len, ==, 1);
  calendar_free (cal);
  g_remove (path);
  g_rmdir (dir);
}

/* a weekly event on Mondays: 5, 12, 19, 26 October 2026 */
static Calendar *
weekly (const char **id_out, gboolean persist_to_disk, char **path_out)
{
  (void) persist_to_disk; (void) path_out;
  Calendar *cal = calendar_new (NULL);
  CalEvent *w = calendar_event_new ("Gym", at (2026, 10, 5, 7, 0), at (2026, 10, 5, 8, 0), FALSE);
  w->repeat = CAL_REPEAT_WEEKLY;
  *id_out = calendar_add (cal, w);
  return cal;
}

static guint
october (Calendar *cal)
{
  return count_in (cal, at (2026, 10, 1, 0, 0), at (2026, 11, 1, 0, 0));
}

static void
test_scopes (void)
{
  const char *id;
  Calendar *cal = weekly (&id, FALSE, NULL);
  g_autofree char *eid = g_strdup (id);
  g_assert_cmpuint (october (cal), ==, 4);

  /* delete only the 12th */
  g_assert_true (calendar_apply_delete (cal, eid, at (2026, 10, 12, 7, 0), CAL_SCOPE_ONE));
  g_assert_cmpuint (october (cal), ==, 3);
  g_assert_cmpuint (count_in (cal, at (2026, 10, 12, 0, 0), at (2026, 10, 13, 0, 0)), ==, 0);
  g_assert_cmpuint (count_in (cal, at (2026, 10, 19, 0, 0), at (2026, 10, 20, 0, 0)), ==, 1);

  /* delete the 19th and after: only the 5th is left */
  g_assert_true (calendar_apply_delete (cal, eid, at (2026, 10, 19, 7, 0), CAL_SCOPE_FUTURE));
  g_assert_cmpuint (october (cal), ==, 1);
  g_assert_cmpuint (count_in (cal, at (2026, 10, 5, 0, 0), at (2026, 10, 6, 0, 0)), ==, 1);
  g_assert_cmpuint (count_in (cal, at (2026, 12, 1, 0, 0), at (2027, 1, 1, 0, 0)), ==, 0);

  /* deleting from the very first showing onward removes the event */
  g_assert_true (calendar_apply_delete (cal, eid, at (2026, 10, 5, 7, 0), CAL_SCOPE_FUTURE));
  g_assert_cmpuint (calendar_count (cal), ==, 0);
  calendar_free (cal);

  /* delete all */
  cal = weekly (&id, FALSE, NULL);
  g_free (eid);
  eid = g_strdup (id);
  g_assert_true (calendar_apply_delete (cal, eid, at (2026, 10, 19, 7, 0), CAL_SCOPE_ALL));
  g_assert_cmpuint (calendar_count (cal), ==, 0);
  calendar_free (cal);
}

static void
test_edit_scopes (void)
{
  const char *id;
  Calendar *cal = weekly (&id, FALSE, NULL);
  g_autofree char *eid = g_strdup (id);

  /* only the 12th, moved to 9:00 and renamed: the others stay as they were */
  CalEvent *one = calendar_event_new ("Yoga", at (2026, 10, 12, 9, 0), at (2026, 10, 12, 10, 0), FALSE);
  g_assert_true (calendar_apply_edit (cal, eid, at (2026, 10, 12, 7, 0), CAL_SCOPE_ONE, one));
  g_assert_cmpuint (october (cal), ==, 4);
  GArray *occ = calendar_occurrences (cal, at (2026, 10, 12, 0, 0), at (2026, 10, 13, 0, 0));
  g_assert_cmpuint (occ->len, ==, 1);
  g_assert_cmpstr (g_array_index (occ, CalOccurrence, 0).event->title, ==, "Yoga");
  g_assert_cmpint (g_array_index (occ, CalOccurrence, 0).start, ==, at (2026, 10, 12, 9, 0));
  g_assert_cmpint (g_array_index (occ, CalOccurrence, 0).event->repeat, ==, CAL_REPEAT_NONE);
  g_array_free (occ, TRUE);
  occ = calendar_occurrences (cal, at (2026, 10, 19, 0, 0), at (2026, 10, 20, 0, 0));
  g_assert_cmpstr (g_array_index (occ, CalOccurrence, 0).event->title, ==, "Gym");
  g_array_free (occ, TRUE);

  /* the 19th and after, one hour later: the 5th stays at 7:00, the rest move to 8:00 */
  CalEvent *future = calendar_event_new ("Gym", at (2026, 10, 19, 8, 0), at (2026, 10, 19, 9, 0), FALSE);
  future->repeat = CAL_REPEAT_WEEKLY;
  g_assert_true (calendar_apply_edit (cal, eid, at (2026, 10, 19, 7, 0), CAL_SCOPE_FUTURE, future));
  occ = calendar_occurrences (cal, at (2026, 10, 1, 0, 0), at (2026, 11, 1, 0, 0));
  gint64 expected[] = { at (2026, 10, 5, 7, 0), at (2026, 10, 12, 9, 0), at (2026, 10, 19, 8, 0), at (2026, 10, 26, 8, 0) };
  g_assert_cmpuint (occ->len, ==, 4);
  for (guint i = 0; i < 4; i++)
    g_assert_cmpint (g_array_index (occ, CalOccurrence, i).start, ==, expected[i]);
  g_array_free (occ, TRUE);
  calendar_free (cal);

  /* all of them: every showing moves, including ones that were not touched */
  cal = weekly (&id, FALSE, NULL);
  g_free (eid);
  eid = g_strdup (id);
  CalEvent *all = calendar_event_new ("Gym", at (2026, 10, 19, 18, 30), at (2026, 10, 19, 19, 30), FALSE);
  all->repeat = CAL_REPEAT_WEEKLY;
  g_assert_true (calendar_apply_edit (cal, eid, at (2026, 10, 19, 7, 0), CAL_SCOPE_ALL, all));
  occ = calendar_occurrences (cal, at (2026, 10, 1, 0, 0), at (2026, 11, 1, 0, 0));
  g_assert_cmpuint (occ->len, ==, 4);
  g_assert_cmpint (g_array_index (occ, CalOccurrence, 0).start, ==, at (2026, 10, 5, 18, 30));
  g_assert_cmpint (g_array_index (occ, CalOccurrence, 3).start, ==, at (2026, 10, 26, 18, 30));
  g_array_free (occ, TRUE);
  calendar_free (cal);
}

static void
test_exceptions_persist (void)
{
  g_autofree char *dir = g_dir_make_tmp ("cal-XXXXXX", NULL);
  g_autofree char *path = g_build_filename (dir, "calendar.json", NULL);
  Calendar *cal = calendar_new (path);
  CalEvent *w = calendar_event_new ("Gym", at (2026, 10, 5, 7, 0), at (2026, 10, 5, 8, 0), FALSE);
  w->repeat = CAL_REPEAT_WEEKLY;
  g_autofree char *eid = g_strdup (calendar_add (cal, w));
  calendar_apply_delete (cal, eid, at (2026, 10, 12, 7, 0), CAL_SCOPE_ONE);
  calendar_free (cal);
  cal = calendar_new (path);
  g_assert_cmpuint (october (cal), ==, 3);
  /* a copy of the event keeps its exceptions */
  CalEvent *copy = calendar_event_copy (calendar_find (cal, eid));
  g_assert_nonnull (copy->exceptions);
  g_assert_cmpuint (copy->exceptions->len, ==, 1);
  calendar_event_free (copy);
  calendar_free (cal);
  g_remove (path);
  g_rmdir (dir);
}

static void
test_alerts (void)
{
  Calendar *cal = calendar_new (NULL);
  CalAlert a;
  g_assert_false (calendar_next_alert (cal, at (2026, 10, 8, 0, 0), &a));
  CalEvent *e1 = calendar_event_new ("Late", at (2026, 10, 9, 15, 0), 0, FALSE);
  calendar_event_add_alert (e1, 30);
  calendar_add (cal, e1);
  CalEvent *e2 = calendar_event_new ("Soon", at (2026, 10, 9, 9, 0), 0, FALSE);
  calendar_event_add_alert (e2, 10);
  calendar_add (cal, e2);
  calendar_add (cal, calendar_event_new ("Silent", at (2026, 10, 8, 12, 0), 0, FALSE));
  /* the earliest alert wins, whatever the order events were added in */
  g_assert_true (calendar_next_alert (cal, at (2026, 10, 8, 0, 0), &a));
  g_assert_cmpstr (a.event->title, ==, "Soon");
  g_assert_cmpint (a.fire, ==, at (2026, 10, 9, 8, 50));
  g_assert_cmpint (a.start, ==, at (2026, 10, 9, 9, 0));
  /* asking again from that moment moves on to the next one, and never repeats the same */
  g_assert_true (calendar_next_alert (cal, a.fire, &a));
  g_assert_cmpstr (a.event->title, ==, "Late");
  g_assert_cmpint (a.fire, ==, at (2026, 10, 9, 14, 30));
  g_assert_false (calendar_next_alert (cal, a.fire, &a));
  /* repeating events alert on each showing */
  CalEvent *w = calendar_event_new ("Weekly", at (2026, 10, 1, 8, 0), 0, FALSE);
  w->repeat = CAL_REPEAT_WEEKLY;
  calendar_event_add_alert (w, 0);
  calendar_add (cal, w);
  g_assert_true (calendar_next_alert (cal, at (2026, 10, 9, 15, 0), &a));
  g_assert_cmpstr (a.event->title, ==, "Weekly");
  g_assert_cmpint (a.fire, ==, at (2026, 10, 15, 8, 0));
  calendar_free (cal);
}

static guint
n_in (Calendar *cal, int y1, int m1, int d1, int y2, int m2, int d2)
{
  return count_in (cal, at (y1, m1, d1, 0, 0), at (y2, m2, d2, 0, 0));
}

static void
test_custom_repeats (void)
{
  Calendar *cal = calendar_new (NULL);
  /* every 2 weeks from Mon 5 Oct: 5th, 19th, then 2 Nov */
  CalEvent *e = calendar_event_new ("Biweekly", at (2026, 10, 5, 9, 0), 0, FALSE);
  e->repeat = CAL_REPEAT_WEEKLY;
  e->interval = 2;
  calendar_add (cal, e);
  g_assert_cmpuint (n_in (cal, 2026, 10, 1, 2026, 11, 1), ==, 2);
  g_assert_cmpuint (n_in (cal, 2026, 10, 12, 2026, 10, 13), ==, 0);
  g_assert_cmpuint (n_in (cal, 2026, 11, 2, 2026, 11, 3), ==, 1);
  calendar_free (cal);

  /* Mon, Wed, Fri: starting Wed 7 Oct the Monday of that week is not a showing */
  cal = calendar_new (NULL);
  e = calendar_event_new ("Gym", at (2026, 10, 7, 7, 0), 0, FALSE);
  e->repeat = CAL_REPEAT_WEEKLY;
  e->weekdays = 1 | 4 | 16;
  calendar_add (cal, e);
  g_assert_cmpuint (n_in (cal, 2026, 10, 5, 2026, 10, 12), ==, 2);      /* Wed 7, Fri 9 */
  g_assert_cmpuint (n_in (cal, 2026, 10, 12, 2026, 10, 19), ==, 3);     /* Mon, Wed, Fri */
  g_assert_cmpuint (n_in (cal, 2026, 10, 5, 2026, 10, 6), ==, 0);
  /* fast-forwarding to a far range keeps the pattern */
  g_assert_cmpuint (n_in (cal, 2027, 3, 1, 2027, 3, 8), ==, 3);
  calendar_free (cal);

  /* ends after 3 showings, with an exception that still counts */
  cal = calendar_new (NULL);
  e = calendar_event_new ("Course", at (2026, 10, 5, 18, 0), 0, FALSE);
  e->repeat = CAL_REPEAT_WEEKLY;
  e->count = 3;
  g_autofree char *cid = g_strdup (calendar_add (cal, e));
  g_assert_cmpuint (n_in (cal, 2026, 1, 1, 2027, 1, 1), ==, 3);
  calendar_apply_delete (cal, cid, at (2026, 10, 12, 18, 0), CAL_SCOPE_ONE);
  g_assert_cmpuint (n_in (cal, 2026, 1, 1, 2027, 1, 1), ==, 2);
  calendar_free (cal);

  /* monthly: the second Tuesday, the last Friday, the last day */
  cal = calendar_new (NULL);
  e = calendar_event_new ("Board", at (2026, 10, 13, 10, 0), 0, FALSE);   /* the 2nd Tuesday */
  e->repeat = CAL_REPEAT_MONTHLY;
  e->monthly = CAL_MONTHLY_WEEKDAY;
  calendar_add (cal, e);
  g_assert_cmpuint (n_in (cal, 2026, 11, 10, 2026, 11, 11), ==, 1);      /* Tue 10 Nov */
  g_assert_cmpuint (n_in (cal, 2026, 12, 8, 2026, 12, 9), ==, 1);        /* Tue 8 Dec */
  e = calendar_event_new ("Close", at (2026, 10, 30, 17, 0), 0, FALSE);   /* the last Friday */
  e->repeat = CAL_REPEAT_MONTHLY;
  e->monthly = CAL_MONTHLY_WEEKDAY;
  calendar_add (cal, e);
  g_assert_cmpuint (n_in (cal, 2026, 11, 27, 2026, 11, 28), ==, 1);      /* Fri 27 Nov */
  e = calendar_event_new ("Rent", at (2026, 10, 31, 9, 0), 0, FALSE);
  e->repeat = CAL_REPEAT_MONTHLY;
  e->monthly = CAL_MONTHLY_LAST_DAY;
  calendar_add (cal, e);
  g_assert_cmpuint (n_in (cal, 2026, 11, 30, 2026, 12, 1), ==, 1);
  g_assert_cmpuint (n_in (cal, 2027, 2, 28, 2027, 3, 1), ==, 1);
  calendar_free (cal);
}

static void
test_multiday_and_reminders (void)
{
  Calendar *cal = calendar_new (NULL);
  /* Mon 5 to Fri 9, all day: shows on each of those days and not after */
  CalEvent *trip = calendar_event_new ("Trip", at (2026, 10, 5, 0, 0), at (2026, 10, 10, 0, 0), TRUE);
  calendar_add (cal, trip);
  for (int d = 5; d <= 9; d++)
    g_assert_cmpuint (n_in (cal, 2026, 10, d, 2026, 10, d + 1), ==, 1);
  g_assert_cmpuint (n_in (cal, 2026, 10, 10, 2026, 10, 11), ==, 0);
  g_assert_cmpuint (n_in (cal, 2026, 10, 4, 2026, 10, 5), ==, 0);

  /* a reminder ticks off once; a repeating one ticks off showing by showing */
  CalEvent *r = calendar_event_new ("Call bank", at (2026, 10, 6, 10, 0), 0, FALSE);
  r->reminder = TRUE;
  g_autofree char *rid = g_strdup (calendar_add (cal, r));
  g_assert_false (calendar_is_done (calendar_find (cal, rid), at (2026, 10, 6, 10, 0)));
  calendar_set_done (cal, rid, at (2026, 10, 6, 10, 0), TRUE);
  g_assert_true (calendar_is_done (calendar_find (cal, rid), at (2026, 10, 6, 10, 0)));
  calendar_set_done (cal, rid, at (2026, 10, 6, 10, 0), FALSE);
  g_assert_false (calendar_is_done (calendar_find (cal, rid), at (2026, 10, 6, 10, 0)));

  CalEvent *w = calendar_event_new ("Water plants", at (2026, 10, 5, 8, 0), 0, FALSE);
  w->reminder = TRUE;
  w->repeat = CAL_REPEAT_WEEKLY;
  g_autofree char *wid = g_strdup (calendar_add (cal, w));
  calendar_set_done (cal, wid, at (2026, 10, 12, 8, 0), TRUE);
  g_assert_true (calendar_is_done (calendar_find (cal, wid), at (2026, 10, 12, 8, 0)));
  g_assert_false (calendar_is_done (calendar_find (cal, wid), at (2026, 10, 19, 8, 0)));
  calendar_free (cal);
}

static void
test_alerts_and_trash (void)
{
  g_autofree char *dir = g_dir_make_tmp ("cal-XXXXXX", NULL);
  g_autofree char *path = g_build_filename (dir, "calendar.json", NULL);
  Calendar *cal = calendar_new (path);
  CalEvent *e = calendar_event_new ("Flight", at (2026, 10, 20, 8, 0), 0, FALSE);
  int mins[] = { 1440, 60, 10 };
  calendar_event_set_alerts (e, mins, 3);
  e->count = 0;
  e->interval = 3;
  e->weekdays = 5;
  g_free (e->url);
  e->url = g_strdup ("https://example.org/check-in");
  g_autofree char *id = g_strdup (calendar_add (cal, e));
  CalAlert a;
  /* only showings in the next ten days are looked at; the timer asks again every hour */
  g_assert_false (calendar_next_alert (cal, at (2026, 10, 1, 0, 0), &a));
  g_assert_true (calendar_next_alert (cal, at (2026, 10, 12, 0, 0), &a));
  g_assert_cmpint (a.fire, ==, at (2026, 10, 19, 8, 0));
  g_assert_true (calendar_next_alert (cal, a.fire, &a));
  g_assert_cmpint (a.fire, ==, at (2026, 10, 20, 7, 0));
  g_assert_true (calendar_next_alert (cal, a.fire, &a));
  g_assert_cmpint (a.fire, ==, at (2026, 10, 20, 7, 50));

  /* a completed reminder does not alert */
  CalEvent *r = calendar_event_new ("Pay", at (2026, 10, 21, 9, 0), 0, FALSE);
  r->reminder = TRUE;
  calendar_event_add_alert (r, 0);
  g_autofree char *rid = g_strdup (calendar_add (cal, r));
  calendar_set_done (cal, rid, at (2026, 10, 21, 9, 0), TRUE);
  g_assert_false (calendar_next_alert (cal, at (2026, 10, 20, 8, 0), &a) && a.fire == at (2026, 10, 21, 9, 0));

  /* all new fields survive a save and load */
  calendar_free (cal);
  cal = calendar_new (path);
  const CalEvent *f = calendar_find (cal, id);
  g_assert_nonnull (f);
  g_assert_cmpuint (calendar_event_alert_count (f), ==, 3);
  g_assert_cmpint (calendar_event_alert (f, 1), ==, 60);
  g_assert_cmpstr (f->url, ==, "https://example.org/check-in");
  g_assert_cmpuint (f->interval, ==, 3);
  g_assert_cmpuint (f->weekdays, ==, 5);
  g_assert_true (calendar_find (cal, rid)->done);

  /* a deletion goes to the trash and comes back, even after a restart */
  g_assert_true (calendar_remove (cal, id));
  g_assert_null (calendar_find (cal, id));
  calendar_free (cal);
  cal = calendar_new (path);
  g_autofree char *title = NULL;
  g_assert_true (calendar_undo_delete (cal, &title));
  g_assert_cmpstr (title, ==, "Flight");
  g_assert_nonnull (calendar_find (cal, id));
  g_assert_false (calendar_undo_delete (cal, NULL));
  calendar_free (cal);
  g_remove (path);
  g_rmdir (dir);
}

static void
test_free_slots_and_subscriptions (void)
{
  Calendar *cal = calendar_new (NULL);
  calendar_add (cal, calendar_event_new ("A", at (2026, 10, 9, 10, 0), at (2026, 10, 9, 11, 30), FALSE));
  calendar_add (cal, calendar_event_new ("B", at (2026, 10, 9, 13, 0), at (2026, 10, 9, 14, 0), FALSE));
  calendar_add (cal, calendar_event_new ("Holiday", at (2026, 10, 9, 0, 0), 0, TRUE));   /* all day events do not block */
  /* 9:00-17:00, gaps of at least an hour: 9-10, 11:30-13, 14-17 */
  GArray *g = calendar_free_slots (cal, at (2026, 10, 9, 0, 0), at (2026, 10, 10, 0, 0), 60, 9, 17);
  g_assert_cmpuint (g->len, ==, 6);
  g_assert_cmpint (g_array_index (g, gint64, 0), ==, at (2026, 10, 9, 9, 0));
  g_assert_cmpint (g_array_index (g, gint64, 1), ==, at (2026, 10, 9, 10, 0));
  g_assert_cmpint (g_array_index (g, gint64, 2), ==, at (2026, 10, 9, 11, 30));
  g_assert_cmpint (g_array_index (g, gint64, 3), ==, at (2026, 10, 9, 13, 0));
  g_assert_cmpint (g_array_index (g, gint64, 4), ==, at (2026, 10, 9, 14, 0));
  g_assert_cmpint (g_array_index (g, gint64, 5), ==, at (2026, 10, 9, 17, 0));
  g_array_free (g, TRUE);
  /* with 2 hours needed only the afternoon gap is left */
  g = calendar_free_slots (cal, at (2026, 10, 9, 0, 0), at (2026, 10, 10, 0, 0), 120, 9, 17);
  g_assert_cmpuint (g->len, ==, 2);
  g_array_free (g, TRUE);

  /* a subscription replaces its events and cannot take new ones */
  const char *sub = calendar_subscription_add (cal, "Holidays", 4, "https://example.org/h.ics", 24);
  g_autofree char *sid = g_strdup (sub);
  GPtrArray *events = g_ptr_array_new_with_free_func ((GDestroyNotify) calendar_event_free);
  g_ptr_array_add (events, calendar_event_new ("Columbus Day", at (2026, 10, 12, 0, 0), 0, TRUE));
  g_ptr_array_add (events, calendar_event_new ("Veterans Day", at (2026, 11, 11, 0, 0), 0, TRUE));
  calendar_replace_events (cal, sid, events);
  g_ptr_array_unref (events);
  g_assert_cmpuint (n_in (cal, 2026, 10, 12, 2026, 10, 13), ==, 1);
  g_assert_true (calendar_calendar_find (cal, sid)->fetched > 0);
  CalEvent *mine = calendar_event_new ("Mine", at (2026, 10, 13, 9, 0), 0, FALSE);
  mine->calendar = g_strdup (sid);
  const char *mid = calendar_add (cal, mine);
  g_assert_cmpstr (calendar_find (cal, mid)->calendar, !=, sid);
  /* refreshing replaces, it does not pile up */
  events = g_ptr_array_new_with_free_func ((GDestroyNotify) calendar_event_free);
  g_ptr_array_add (events, calendar_event_new ("Columbus Day", at (2026, 10, 12, 0, 0), 0, TRUE));
  calendar_replace_events (cal, sid, events);
  g_ptr_array_unref (events);
  g_assert_cmpuint (n_in (cal, 2026, 10, 12, 2026, 10, 13), ==, 1);
  g_assert_cmpuint (n_in (cal, 2026, 11, 11, 2026, 11, 12), ==, 0);
  /* the user's last own calendar cannot be removed even when subscriptions exist */
  g_assert_false (calendar_calendar_remove (cal, ((CalCalendar *) calendar_calendars (cal)->pdata[0])->id));
  g_assert_true (calendar_calendar_remove (cal, sid));
  calendar_free (cal);
}

static void
test_scope_with_count (void)
{
  Calendar *cal = calendar_new (NULL);
  CalEvent *e = calendar_event_new ("Class", at (2026, 10, 5, 18, 0), 0, FALSE);
  e->repeat = CAL_REPEAT_WEEKLY;
  e->count = 6;
  g_autofree char *id = g_strdup (calendar_add (cal, e));
  /* from the 4th showing on, change the title: the new series has the 3 that were left */
  CalEvent *edited = calendar_event_new ("Class (new room)", at (2026, 10, 26, 18, 0), at (2026, 10, 26, 19, 0), FALSE);
  edited->repeat = CAL_REPEAT_WEEKLY;
  g_assert_true (calendar_apply_edit (cal, id, at (2026, 10, 26, 18, 0), CAL_SCOPE_FUTURE, edited));
  g_assert_cmpuint (n_in (cal, 2026, 1, 1, 2027, 6, 1), ==, 6);
  g_assert_cmpuint (n_in (cal, 2026, 10, 5, 2026, 10, 26), ==, 3);
  calendar_free (cal);
}

static void
test_sync_model (void)
{
  g_autofree char *dir = g_dir_make_tmp ("cal-XXXXXX", NULL);
  g_autofree char *path = g_build_filename (dir, "calendar.json", NULL);
  Calendar *cal = calendar_new (path);
  g_autofree char *local = g_strdup (((CalCalendar *) calendar_calendars (cal)->pdata[0])->id);
  g_autofree char *icloud = g_strdup (calendar_linked_add (cal, "iCloud", 3, "acct-1", "/u/calendars/home/"));

  /* events in a plain local calendar never need syncing */
  CalEvent *l = calendar_event_new ("Local", at (2026, 10, 9, 9, 0), 0, FALSE);
  l->calendar = g_strdup (local);
  g_autofree char *lid = g_strdup (calendar_add (cal, l));
  g_assert_false (calendar_find (cal, lid)->dirty);

  /* a new event in the synced calendar is dirty and has an identity */
  CalEvent *e = calendar_event_new ("Dentist", at (2026, 10, 9, 15, 0), 0, FALSE);
  e->calendar = g_strdup (icloud);
  g_autofree char *eid = g_strdup (calendar_add (cal, e));
  g_assert_true (calendar_find (cal, eid)->dirty);
  g_assert_nonnull (calendar_find (cal, eid)->uid);
  g_autofree char *uid = g_strdup (calendar_find (cal, eid)->uid);
  GPtrArray *dirty = calendar_dirty_events (cal, icloud);
  g_assert_cmpuint (dirty->len, ==, 1);
  g_ptr_array_unref (dirty);

  /* after the upload it is clean and knows where it lives */
  calendar_mark_synced (cal, icloud, uid, "/u/calendars/home/abc.ics", "\"etag-1\"");
  g_assert_false (calendar_find (cal, eid)->dirty);
  g_assert_cmpstr (calendar_find (cal, eid)->href, ==, "/u/calendars/home/abc.ics");

  /* an edit from the editor arrives as a fresh copy: it keeps its identity and becomes dirty */
  CalEvent *edit = calendar_event_new ("Dentist (new time)", at (2026, 10, 9, 16, 0), 0, FALSE);
  edit->id = g_strdup (eid);
  edit->calendar = g_strdup (icloud);
  g_assert_true (calendar_update (cal, edit));
  const CalEvent *after = calendar_find (cal, eid);
  g_assert_true (after->dirty);
  g_assert_cmpstr (after->uid, ==, uid);
  g_assert_cmpstr (after->href, ==, "/u/calendars/home/abc.ics");
  g_assert_cmpstr (after->etag, ==, "\"etag-1\"");
  calendar_mark_synced (cal, icloud, uid, "/u/calendars/home/abc.ics", "\"etag-2\"");

  /* a repeating one: changing one showing makes a real override of the same series */
  CalEvent *w = calendar_event_new ("Gym", at (2026, 10, 5, 7, 0), at (2026, 10, 5, 8, 0), FALSE);
  w->calendar = g_strdup (icloud);
  w->repeat = CAL_REPEAT_WEEKLY;
  g_autofree char *wid = g_strdup (calendar_add (cal, w));
  g_autofree char *wuid = g_strdup (calendar_find (cal, wid)->uid);
  calendar_mark_synced (cal, icloud, wuid, "/u/calendars/home/gym.ics", "\"g1\"");
  CalEvent *moved = calendar_event_new ("Gym (late)", at (2026, 10, 12, 9, 0), at (2026, 10, 12, 10, 0), FALSE);
  g_assert_true (calendar_apply_edit (cal, wid, at (2026, 10, 12, 7, 0), CAL_SCOPE_ONE, moved));
  GPtrArray *group = calendar_events_with_uid (cal, icloud, wuid);
  g_assert_cmpuint (group->len, ==, 2);
  const CalEvent *over = NULL;
  for (guint i = 0; i < group->len; i++)
    if (((CalEvent *) group->pdata[i])->recurrence_id)
      over = group->pdata[i];
  g_assert_nonnull (over);
  g_assert_cmpint (over->recurrence_id, ==, at (2026, 10, 12, 7, 0));
  g_assert_cmpstr (over->href, ==, "/u/calendars/home/gym.ics");
  g_assert_true (calendar_find (cal, wid)->dirty);              /* the series got an exception */
  g_ptr_array_unref (group);
  calendar_mark_synced (cal, icloud, wuid, "/u/calendars/home/gym.ics", "\"g2\"");

  /* deleting the series takes its changed showings and queues one deletion on the server */
  g_assert_true (calendar_remove (cal, wid));
  GPtrArray *gone = calendar_events_with_uid (cal, icloud, wuid);
  g_assert_cmpuint (gone->len, ==, 0);
  g_ptr_array_unref (gone);
  const GPtrArray *pending = calendar_pending_deletes (cal);
  g_assert_cmpuint (pending->len, ==, 1);
  g_assert_cmpstr (((CalPendingDelete *) pending->pdata[0])->href, ==, "/u/calendars/home/gym.ics");
  g_assert_cmpstr (((CalPendingDelete *) pending->pdata[0])->etag, ==, "\"g2\"");

  /* everything survives a restart */
  calendar_free (cal);
  cal = calendar_new (path);
  g_assert_cmpuint (calendar_pending_deletes (cal)->len, ==, 1);
  g_assert_cmpstr (calendar_calendar_find (cal, icloud)->account, ==, "acct-1");
  g_assert_cmpstr (calendar_calendar_find (cal, icloud)->href, ==, "/u/calendars/home/");
  g_assert_cmpstr (calendar_find (cal, eid)->etag, ==, "\"etag-2\"");

  /* undoing the deletion cancels it */
  g_autofree char *title = NULL;
  g_assert_true (calendar_undo_delete (cal, &title));
  g_assert_cmpuint (calendar_pending_deletes (cal)->len, ==, 0);
  g_assert_true (calendar_find (cal, wid)->dirty);

  /* the server's version of a resource replaces ours without queueing anything */
  GPtrArray *fresh = g_ptr_array_new_with_free_func ((GDestroyNotify) calendar_event_free);
  CalEvent *srv = calendar_event_new ("Dentist (server)", at (2026, 10, 9, 17, 0), 0, FALSE);
  srv->uid = g_strdup (uid);
  srv->etag = g_strdup ("\"etag-3\"");
  g_ptr_array_add (fresh, srv);
  calendar_replace_resource (cal, icloud, "/u/calendars/home/abc.ics", fresh);
  g_ptr_array_unref (fresh);
  GPtrArray *same = calendar_events_with_uid (cal, icloud, uid);
  g_assert_cmpuint (same->len, ==, 1);
  g_assert_cmpstr (((CalEvent *) same->pdata[0])->title, ==, "Dentist (server)");
  g_assert_false (((CalEvent *) same->pdata[0])->dirty);
  g_assert_cmpstr (((CalEvent *) same->pdata[0])->href, ==, "/u/calendars/home/abc.ics");
  g_ptr_array_unref (same);
  calendar_remove_resource (cal, icloud, "/u/calendars/home/abc.ics");
  GPtrArray *gone2 = calendar_events_with_uid (cal, icloud, uid);
  g_assert_cmpuint (gone2->len, ==, 0);
  g_ptr_array_unref (gone2);

  /* moving an event to another calendar deletes it from the old one on the server */
  CalEvent *mv = calendar_event_new ("Move me", at (2026, 10, 10, 9, 0), 0, FALSE);
  mv->calendar = g_strdup (icloud);
  g_autofree char *mid = g_strdup (calendar_add (cal, mv));
  calendar_mark_synced (cal, icloud, calendar_find (cal, mid)->uid, "/u/calendars/home/move.ics", "\"m1\"");
  guint before = calendar_pending_deletes (cal)->len;
  CalEvent *to_local = calendar_event_new ("Move me", at (2026, 10, 10, 9, 0), 0, FALSE);
  to_local->id = g_strdup (mid);
  to_local->calendar = g_strdup (local);
  g_assert_true (calendar_update (cal, to_local));
  g_assert_cmpuint (calendar_pending_deletes (cal)->len, ==, before + 1);
  g_assert_false (calendar_find (cal, mid)->dirty);                /* it is a local event now */
  g_assert_null (calendar_find (cal, mid)->href);

  /* stopping the sync keeps the events as local ones */
  calendar_unlink_account (cal, "acct-1", FALSE);
  g_assert_null (calendar_calendar_find (cal, icloud)->account);
  g_assert_cmpuint (calendar_pending_deletes (cal)->len, ==, 0);
  calendar_free (cal);
  g_remove (path);
  g_rmdir (dir);
}

static void
test_where_new_events_go (void)
{
  Calendar *cal = calendar_new (NULL);
  g_autofree char *local = g_strdup (((CalCalendar *) calendar_calendars (cal)->pdata[0])->id);
  g_autofree char *icloud = g_strdup (calendar_linked_add (cal, "iCloud", 3, "acct-1", "/u/home/"));
  const char *sub = calendar_subscription_add (cal, "Holidays", 4, "https://example.org/h.ics", 24);
  g_autofree char *sub_id = g_strdup (sub);

  /* both visible: a calendar kept in step with a server wins, so the event reaches the phone */
  g_assert_cmpstr (calendar_preferred_target (cal, NULL)->id, ==, icloud);
  /* the user's own choice wins when it takes events */
  g_assert_cmpstr (calendar_preferred_target (cal, local)->id, ==, local);
  /* a subscription is never a target, even if asked for */
  g_assert_cmpstr (calendar_preferred_target (cal, sub_id)->id, ==, icloud);
  /* a hidden one is not chosen for you */
  calendar_calendar_find (cal, icloud)->visible = FALSE;
  g_assert_cmpstr (calendar_preferred_target (cal, NULL)->id, ==, local);
  calendar_calendar_find (cal, local)->visible = FALSE;
  g_assert_nonnull (calendar_preferred_target (cal, NULL));            /* still somewhere to put it */

  /* an event added with no calendar lands in a visible one, and nothing vanishes */
  calendar_calendar_find (cal, local)->visible = TRUE;
  calendar_calendar_find (cal, icloud)->visible = FALSE;
  const char *id = calendar_add (cal, calendar_event_new ("No calendar given", at (2026, 10, 9, 9, 0), 0, FALSE));
  g_assert_cmpstr (calendar_find (cal, id)->calendar, ==, local);

  /* adding to a calendar you had hidden shows it again: the event is seen */
  CalEvent *e = calendar_event_new ("Into the hidden one", at (2026, 10, 9, 10, 0), 0, FALSE);
  e->calendar = g_strdup (icloud);
  g_assert_false (calendar_calendar_find (cal, icloud)->visible);
  calendar_add (cal, e);
  g_assert_true (calendar_calendar_find (cal, icloud)->visible);

  /* moving an event into a hidden calendar shows it too */
  calendar_calendar_find (cal, local)->visible = FALSE;
  CalEvent *move = calendar_event_new ("No calendar given", at (2026, 10, 9, 9, 0), 0, FALSE);
  move->id = g_strdup (id);
  move->calendar = g_strdup (local);
  g_assert_true (calendar_update (cal, move));
  g_assert_true (calendar_calendar_find (cal, local)->visible);
  calendar_free (cal);
}

static void
test_parse (void)
{
  gint64 t;
  gboolean date_only;
  g_assert_true (calendar_parse_time ("2026-10-09T15:30", &t, &date_only));
  g_assert_false (date_only);
  g_assert_cmpint (t, ==, at (2026, 10, 9, 15, 30));
  g_assert_true (calendar_parse_time ("2026-10-09", &t, &date_only));
  g_assert_true (date_only);
  g_assert_true (calendar_parse_time ("2026-10-09 08:05", &t, NULL));
  g_assert_false (calendar_parse_time ("tomorrow", &t, NULL));
  g_assert_false (calendar_parse_time ("2026-13-40", &t, NULL));
  g_assert_false (calendar_parse_time (NULL, &t, NULL));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/calendar/single", test_single);
  g_test_add_func ("/calendar/repeat", test_repeat);
  g_test_add_func ("/calendar/bounded", test_bounded);
  g_test_add_func ("/calendar/roundtrip", test_sorted_and_roundtrip);
  g_test_add_func ("/calendar/edit", test_edit);
  g_test_add_func ("/calendar/calendars", test_calendars);
  g_test_add_func ("/calendar/scopes", test_scopes);
  g_test_add_func ("/calendar/edit-scopes", test_edit_scopes);
  g_test_add_func ("/calendar/exceptions", test_exceptions_persist);
  g_test_add_func ("/calendar/alerts", test_alerts);
  g_test_add_func ("/calendar/custom-repeats", test_custom_repeats);
  g_test_add_func ("/calendar/multiday-reminders", test_multiday_and_reminders);
  g_test_add_func ("/calendar/alerts-trash", test_alerts_and_trash);
  g_test_add_func ("/calendar/free-slots-subscriptions", test_free_slots_and_subscriptions);
  g_test_add_func ("/calendar/scope-count", test_scope_with_count);
  g_test_add_func ("/calendar/sync-model", test_sync_model);
  g_test_add_func ("/calendar/where-new-events-go", test_where_new_events_go);
  g_test_add_func ("/calendar/parse", test_parse);
  return g_test_run ();
}
