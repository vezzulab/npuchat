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
  g_test_add_func ("/calendar/parse", test_parse);
  return g_test_run ();
}
