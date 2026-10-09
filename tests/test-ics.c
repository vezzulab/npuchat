#include <glib.h>
#include <string.h>

#include "../src/calendar-ics.h"
#include "../src/calendar.h"

static gint64
at (int y, int mo, int d, int h, int mi)
{
  g_autoptr (GDateTime) dt = g_date_time_new_local (y, mo, d, h, mi, 0);
  return g_date_time_to_unix (dt);
}

static gint64
utc (int y, int mo, int d, int h, int mi)
{
  g_autoptr (GDateTime) dt = g_date_time_new_utc (y, mo, d, h, mi, 0);
  return g_date_time_to_unix (dt);
}

static const CalEvent *
by_title (GPtrArray *events, const char *title)
{
  for (guint i = 0; i < events->len; i++)
    if (g_str_equal (((CalEvent *) events->pdata[i])->title, title))
      return events->pdata[i];
  return NULL;
}

/* what iPhones, Google and Outlook actually send */
static const char *sample =
  "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//Apple Inc.//iPhone//EN\r\n"
  "BEGIN:VEVENT\r\nUID:aaa-1\r\nDTSTAMP:20260101T000000Z\r\n"
  "DTSTART:20261009T190000Z\r\nDTEND:20261009T210000Z\r\n"
  "SUMMARY:Dinner\\, with Ana\\; and Luis\r\nLOCATION:Caf\xc3\xa9 Central\r\n"
  "DESCRIPTION:Bring the documents\\nand a very long line that continues\r\n  on the next line after folding\r\n"
  "URL:https://example.org/menu\r\n"
  "BEGIN:VALARM\r\nACTION:DISPLAY\r\nTRIGGER:-PT30M\r\nEND:VALARM\r\n"
  "BEGIN:VALARM\r\nACTION:DISPLAY\r\nTRIGGER:-P1D\r\nEND:VALARM\r\n"
  "END:VEVENT\r\n"
  "BEGIN:VEVENT\r\nUID:aaa-2\r\nDTSTART;VALUE=DATE:20261012\r\nDTEND;VALUE=DATE:20261015\r\nSUMMARY:Trip\r\nEND:VEVENT\r\n"
  "BEGIN:VEVENT\r\nUID:aaa-3\r\nDTSTART;TZID=America/New_York:20261005T090000\r\nDTEND;TZID=America/New_York:20261005T100000\r\n"
  "SUMMARY:Standup\r\nRRULE:FREQ=WEEKLY;INTERVAL=2;BYDAY=MO,WE;COUNT=6\r\n"
  "EXDATE;TZID=America/New_York:20261007T090000\r\nEND:VEVENT\r\n"
  "BEGIN:VEVENT\r\nUID:aaa-4\r\nDTSTART:20261013T140000\r\nSUMMARY:Board\r\nRRULE:FREQ=MONTHLY;BYDAY=2TU;UNTIL=20271231T000000Z\r\nEND:VEVENT\r\n"
  "BEGIN:VEVENT\r\nUID:aaa-5\r\nDTSTART:20261031T170000\r\nDURATION:PT90M\r\nSUMMARY:Rent\r\nRRULE:FREQ=MONTHLY;BYMONTHDAY=-1\r\nEND:VEVENT\r\n"
  "BEGIN:VEVENT\r\nUID:aaa-6\r\nDTSTART:20261101T100000\r\nSUMMARY:Cancelled thing\r\nSTATUS:CANCELLED\r\nEND:VEVENT\r\n"
  "BEGIN:VEVENT\r\nSUMMARY:No start, ignored\r\nEND:VEVENT\r\n"
  "BEGIN:VTODO\r\nUID:todo-1\r\nDUE:20261020T100000\r\nSUMMARY:Pay rent\r\nSTATUS:COMPLETED\r\nEND:VTODO\r\n"
  /* a changed single showing of the weekly standup: Monday 19 Oct moved to 15:00 */
  "BEGIN:VEVENT\r\nUID:aaa-3\r\nRECURRENCE-ID;TZID=America/New_York:20261019T090000\r\n"
  "DTSTART;TZID=America/New_York:20261019T150000\r\nDTEND;TZID=America/New_York:20261019T160000\r\nSUMMARY:Standup (moved)\r\nEND:VEVENT\r\n"
  "END:VCALENDAR\r\n";

static void
test_parse (void)
{
  g_autoptr (GPtrArray) ev = calendar_ics_parse (sample);
  /* 5 events + the moved showing + the to-do; the cancelled and the start-less ones are dropped */
  g_assert_cmpuint (ev->len, ==, 7);

  const CalEvent *dinner = by_title (ev, "Dinner, with Ana; and Luis");
  g_assert_nonnull (dinner);
  g_assert_cmpint (dinner->start, ==, utc (2026, 10, 9, 19, 0));
  g_assert_cmpint (dinner->end, ==, utc (2026, 10, 9, 21, 0));
  g_assert_cmpstr (dinner->location, ==, "Caf\xc3\xa9 Central");
  g_assert_nonnull (strstr (dinner->notes, "Bring the documents\nand a very long line that continues on the next line"));
  g_assert_cmpstr (dinner->url, ==, "https://example.org/menu");
  g_assert_cmpuint (calendar_event_alert_count (dinner), ==, 2);
  g_assert_cmpint (calendar_event_alert (dinner, 0), ==, 30);
  g_assert_cmpint (calendar_event_alert (dinner, 1), ==, 1440);
  g_assert_cmpstr (dinner->uid, ==, "aaa-1");

  const CalEvent *trip = by_title (ev, "Trip");
  g_assert_true (trip->all_day);
  g_assert_cmpint (trip->start, ==, at (2026, 10, 12, 0, 0));
  g_assert_cmpint (trip->end, ==, at (2026, 10, 15, 0, 0));       /* the last day is the 14th */

  const CalEvent *standup = by_title (ev, "Standup");
  g_autoptr (GTimeZone) ny = g_time_zone_new_identifier ("America/New_York");
  g_autoptr (GDateTime) ny_start = g_date_time_new (ny, 2026, 10, 5, 9, 0, 0);
  g_assert_cmpint (standup->start, ==, g_date_time_to_unix (ny_start));
  g_assert_cmpint (standup->repeat, ==, CAL_REPEAT_WEEKLY);
  g_assert_cmpuint (standup->interval, ==, 2);
  g_assert_cmpuint (standup->weekdays, ==, 1 | 4);                /* Monday and Wednesday */
  g_assert_cmpint (standup->count, ==, 6);
  g_assert_cmpuint (standup->exceptions->len, ==, 2);             /* the EXDATE and the moved showing */

  const CalEvent *board = by_title (ev, "Board");
  g_assert_cmpint (board->repeat, ==, CAL_REPEAT_MONTHLY);
  g_assert_cmpint (board->monthly, ==, CAL_MONTHLY_WEEKDAY);
  /* UNTIL is a UTC moment: the last day is whichever local day that moment falls on */
  g_assert_cmpint (board->until, ==, calendar_day_start (utc (2027, 12, 31, 0, 0)));
  g_assert_cmpint (board->start, ==, at (2026, 10, 13, 14, 0));    /* a floating time stays on the local clock */

  const CalEvent *rent = by_title (ev, "Rent");
  g_assert_cmpint (rent->monthly, ==, CAL_MONTHLY_LAST_DAY);
  g_assert_cmpint (rent->end - rent->start, ==, 90 * 60);

  const CalEvent *todo = by_title (ev, "Pay rent");
  g_assert_true (todo->reminder);
  g_assert_true (todo->done);
  g_assert_cmpint (todo->start, ==, at (2026, 10, 20, 10, 0));

  g_assert_nonnull (by_title (ev, "Standup (moved)"));
  g_assert_null (by_title (ev, "Cancelled thing"));
}

static void
test_roundtrip (void)
{
  Calendar *cal = calendar_new (NULL);
  CalEvent *a = calendar_event_new ("Weekly, \"quoted\"; with \\ slash", at (2026, 10, 5, 7, 30), at (2026, 10, 5, 8, 30), FALSE);
  a->repeat = CAL_REPEAT_WEEKLY;
  a->weekdays = 1 | 8 | 32;
  a->interval = 2;
  a->count = 8;
  g_free (a->location);
  a->location = g_strdup ("Gym \xe2\x80\x94 \xc3\xb1" "and\xc3\xba");
  g_free (a->notes);
  a->notes = g_strdup ("line one\nline two with a very long tail that has to be folded because it is longer than seventy five bytes");
  calendar_event_add_alert (a, 15);
  calendar_event_add_alert (a, 1440);
  gint64 ex = at (2026, 10, 19, 7, 30);
  a->exceptions = g_array_new (FALSE, FALSE, sizeof (gint64));
  g_array_append_val (a->exceptions, ex);
  calendar_add (cal, a);
  CalEvent *b = calendar_event_new ("Holiday", at (2026, 12, 24, 0, 0), at (2026, 12, 27, 0, 0), TRUE);
  calendar_add (cal, b);
  CalEvent *c = calendar_event_new ("Water plants", at (2026, 10, 6, 9, 0), 0, FALSE);
  c->reminder = TRUE;
  c->done = TRUE;
  calendar_add (cal, c);
  CalEvent *d = calendar_event_new ("Rent", at (2026, 10, 31, 9, 0), 0, FALSE);
  d->repeat = CAL_REPEAT_MONTHLY;
  d->monthly = CAL_MONTHLY_LAST_DAY;
  d->until = at (2027, 6, 30, 0, 0);
  calendar_add (cal, d);

  g_autofree char *text = calendar_ics_export (cal, NULL);
  /* every line obeys the 75-byte limit and uses CRLF */
  g_auto (GStrv) lines = g_strsplit (text, "\r\n", -1);
  for (int i = 0; lines[i]; i++)
    g_assert_cmpuint (strlen (lines[i]), <=, 75);
  g_assert_null (strstr (text, "\n\n"));
  g_assert_true (g_str_has_prefix (text, "BEGIN:VCALENDAR\r\n"));

  g_autoptr (GPtrArray) back = calendar_ics_parse (text);
  g_assert_cmpuint (back->len, ==, 4);

  /* importing into a fresh calendar keeps everything and saves once */
  {
    Calendar *other = calendar_new (NULL);
    g_autoptr (GPtrArray) again = calendar_ics_parse (text);
    g_assert_cmpuint (calendar_import (other, again, ((CalCalendar *) calendar_calendars (other)->pdata[0])->id), ==, 4);
    g_assert_cmpuint (again->len, ==, 0);
    g_assert_cmpuint (calendar_count (other), ==, 4);
    calendar_free (other);
  }
  const CalEvent *a2 = by_title (back, "Weekly, \"quoted\"; with \\ slash");
  g_assert_nonnull (a2);
  g_assert_cmpint (a2->start, ==, a->start);
  g_assert_cmpint (a2->end, ==, a->end);
  g_assert_cmpuint (a2->weekdays, ==, a->weekdays);
  g_assert_cmpuint (a2->interval, ==, 2);
  g_assert_cmpint (a2->count, ==, 8);
  g_assert_cmpstr (a2->location, ==, a->location);
  g_assert_cmpstr (a2->notes, ==, a->notes);
  g_assert_cmpuint (calendar_event_alert_count (a2), ==, 2);
  g_assert_cmpuint (a2->exceptions->len, ==, 1);
  g_assert_cmpint (g_array_index (a2->exceptions, gint64, 0), ==, ex);
  const CalEvent *b2 = by_title (back, "Holiday");
  g_assert_true (b2->all_day);
  g_assert_cmpint (b2->end, ==, b->end);
  const CalEvent *c2 = by_title (back, "Water plants");
  g_assert_true (c2->reminder && c2->done);
  const CalEvent *d2 = by_title (back, "Rent");
  g_assert_cmpint (d2->monthly, ==, CAL_MONTHLY_LAST_DAY);
  g_assert_cmpint (d2->until, ==, d->until);

  /* exporting one calendar only */
  const char *other = calendar_calendar_add (cal, "Work", 2);
  g_autofree char *other_id = g_strdup (other);
  CalEvent *w = calendar_event_new ("Work thing", at (2026, 10, 7, 9, 0), 0, FALSE);
  w->calendar = g_strdup (other_id);
  calendar_add (cal, w);
  g_autofree char *only = calendar_ics_export (cal, other_id);
  g_assert_nonnull (strstr (only, "Work thing"));
  g_assert_null (strstr (only, "Holiday"));
  calendar_free (cal);
}

static void
test_server_resource (void)
{
  /* a weekly series with one changed showing and an exception, as one resource */
  CalEvent *series = calendar_event_new ("Standup", at (2026, 10, 5, 9, 0), at (2026, 10, 5, 9, 30), FALSE);
  series->uid = g_strdup ("series-1");
  series->repeat = CAL_REPEAT_WEEKLY;
  gint64 ex = at (2026, 10, 26, 9, 0), moved_from = at (2026, 10, 12, 9, 0);
  series->exceptions = g_array_new (FALSE, FALSE, sizeof (gint64));
  g_array_append_val (series->exceptions, moved_from);
  g_array_append_val (series->exceptions, ex);
  calendar_event_add_alert (series, 10);
  CalEvent *over = calendar_event_new ("Standup (late)", at (2026, 10, 12, 11, 0), at (2026, 10, 12, 11, 30), FALSE);
  over->uid = g_strdup ("series-1");
  over->recurrence_id = moved_from;
  g_autoptr (GPtrArray) group = g_ptr_array_new_with_free_func ((GDestroyNotify) calendar_event_free);
  g_ptr_array_add (group, over);      /* the order does not matter: the series is written first */
  g_ptr_array_add (group, series);

  g_autofree char *text = calendar_ics_export_events (group);
  /* a series keeps its clock time across daylight saving: no zone, no Z */
  g_assert_nonnull (strstr (text, "DTSTART:20261005T090000\r\n"));
  g_assert_null (strstr (text, "DTSTART:20261005T090000Z"));
  g_assert_nonnull (strstr (text, "RECURRENCE-ID:20261012T090000\r\n"));
  g_assert_nonnull (strstr (text, "EXDATE:20261026T090000\r\n"));
  g_assert_true (strstr (text, "RRULE:FREQ=WEEKLY") < strstr (text, "RECURRENCE-ID"));

  g_autoptr (GPtrArray) back = calendar_ics_parse (text);
  g_assert_cmpuint (back->len, ==, 2);
  const CalEvent *s2 = by_title (back, "Standup");
  const CalEvent *o2 = by_title (back, "Standup (late)");
  g_assert_nonnull (s2);
  g_assert_nonnull (o2);
  g_assert_cmpstr (s2->uid, ==, "series-1");
  g_assert_cmpstr (o2->uid, ==, "series-1");
  g_assert_cmpint (o2->recurrence_id, ==, moved_from);
  g_assert_cmpint (s2->recurrence_id, ==, 0);
  g_assert_cmpint (o2->start, ==, at (2026, 10, 12, 11, 0));
  g_assert_cmpuint (s2->exceptions->len, ==, 2);
  g_assert_cmpint (calendar_event_alert (s2, 0), ==, 10);

  /* a single event keeps its absolute moment; a reminder is an event marked as one */
  CalEvent *single = calendar_event_new ("Dentist", at (2026, 10, 9, 15, 0), at (2026, 10, 9, 16, 0), FALSE);
  single->uid = g_strdup ("single-1");
  CalEvent *rem = calendar_event_new ("Pay rent", at (2026, 10, 9, 10, 0), 0, FALSE);
  rem->uid = g_strdup ("rem-1");
  rem->reminder = TRUE;
  rem->done = TRUE;
  g_autoptr (GPtrArray) one = g_ptr_array_new_with_free_func ((GDestroyNotify) calendar_event_free);
  g_ptr_array_add (one, single);
  g_autofree char *t1 = calendar_ics_export_events (one);
  g_assert_nonnull (strstr (t1, "DTSTART:2026"));
  g_assert_nonnull (strstr (t1, "Z\r\n"));
  g_autoptr (GPtrArray) two = g_ptr_array_new_with_free_func ((GDestroyNotify) calendar_event_free);
  g_ptr_array_add (two, rem);
  g_autofree char *t2 = calendar_ics_export_events (two);
  g_assert_nonnull (strstr (t2, "BEGIN:VEVENT"));
  g_assert_null (strstr (t2, "VTODO"));
  g_assert_nonnull (strstr (t2, "X-CALENDAR-REMINDER:TRUE"));
  g_autoptr (GPtrArray) rem_back = calendar_ics_parse (t2);
  g_assert_cmpuint (rem_back->len, ==, 1);
  g_assert_true (((CalEvent *) rem_back->pdata[0])->reminder);
  g_assert_true (((CalEvent *) rem_back->pdata[0])->done);
  g_assert_cmpint (((CalEvent *) rem_back->pdata[0])->end - ((CalEvent *) rem_back->pdata[0])->start, ==, 900);
}

static void
test_hostile (void)
{
  /* none of these may crash, hang or leak; most produce nothing */
  const char *junk[] = {
    "", "BEGIN:VCALENDAR", "BEGIN:VEVENT\r\nDTSTART:garbage\r\nEND:VEVENT\r\n", "BEGIN:VEVENT",
    "BEGIN:VEVENT\r\nDTSTART:20269999T999999\r\nSUMMARY:x\r\nEND:VEVENT\r\n",
    "BEGIN:VEVENT\r\nDTSTART:20261009T100000\r\nRRULE:FREQ=WEEKLY;INTERVAL=-5;COUNT=99999999999\r\nEND:VEVENT\r\n",
    "BEGIN:VEVENT\r\nDTSTART:20261009T100000\r\nDTEND:19000101T000000\r\nEND:VEVENT\r\n",
    "BEGIN:VEVENT\r\nDTSTART:20261009T100000\r\nDURATION:P99999D\r\nEND:VEVENT\r\n",
    "BEGIN:VEVENT\r\nDTSTART;TZID=Not/AZone:20261009T100000\r\nEND:VEVENT\r\n",
    ":::::\r\n;;;;\r\nEND:VEVENT\r\n", "\xff\xfe\x00garbage",
  };
  for (guint i = 0; i < G_N_ELEMENTS (junk); i++)
    {
      g_autoptr (GPtrArray) ev = calendar_ics_parse (junk[i]);
      for (guint k = 0; k < ev->len; k++)
        g_assert_cmpint (((CalEvent *) ev->pdata[k])->end, >, ((CalEvent *) ev->pdata[k])->start);
    }
  g_autoptr (GPtrArray) none = calendar_ics_parse (NULL);
  g_assert_cmpuint (none->len, ==, 0);

  /* a huge file is cut at a sane number of events instead of eating memory */
  GString *big = g_string_new ("BEGIN:VCALENDAR\r\n");
  for (int i = 0; i < 25000; i++)
    g_string_append (big, "BEGIN:VEVENT\r\nDTSTART:20261009T100000Z\r\nSUMMARY:x\r\nEND:VEVENT\r\n");
  g_string_append (big, "END:VCALENDAR\r\n");
  g_autoptr (GPtrArray) many = calendar_ics_parse (big->str);
  g_assert_cmpuint (many->len, <=, 20000);
  g_string_free (big, TRUE);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/ics/parse", test_parse);
  g_test_add_func ("/ics/roundtrip", test_roundtrip);
  g_test_add_func ("/ics/server-resource", test_server_resource);
  g_test_add_func ("/ics/hostile", test_hostile);
  return g_test_run ();
}
