/* Two "devices" (two calendars with their own data) sync through one real CalDAV server.
 * The server is Radicale in a container: tests/run-caldav-test.sh starts it and runs this.
 * Run by hand: CALDAV_URL=http://127.0.0.1:5233 ./test-caldav */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "../src/calendar-caldav.h"
#include "../src/calendar.h"
#include "../src/net.h"

static int failures;

#define CHECK(cond, ...) \
  G_STMT_START { \
    if (!(cond)) { \
      failures++; \
      g_printerr ("FAIL line %d: ", __LINE__); \
      g_printerr (__VA_ARGS__); \
      g_printerr ("\n"); \
    } \
  } G_STMT_END

#ifdef NPU_CHAT_SELFTEST
gpointer selftest_track (gpointer object, const char *what) { (void) what; return object; }
#endif

static gint64
at (int y, int mo, int d, int h, int mi)
{
  g_autoptr (GDateTime) dt = g_date_time_new_local (y, mo, d, h, mi, 0);
  return g_date_time_to_unix (dt);
}

static gboolean
on_timeout (gpointer loop)
{
  g_main_loop_quit (loop);
  return G_SOURCE_REMOVE;
}

typedef struct {
  GMainLoop *loop;
  char      *error;
  GPtrArray *cals;
  gboolean   finished;
} Wait;

static void
wait_sync_done (const char *error, gpointer data)
{
  Wait *w = data;
  w->error = g_strdup (error);
  w->finished = TRUE;
  g_main_loop_quit (w->loop);
}

/* a sync, with a time limit so a hang is a failure and not a stuck test */
static char *
sync_and_wait (void)
{
  Wait w = { g_main_loop_new (NULL, FALSE), NULL, NULL, FALSE };
  caldav_sync_now (wait_sync_done, &w);
  if (!w.finished)
    {
      guint limit = g_timeout_add_seconds (60, on_timeout, w.loop);
      g_main_loop_run (w.loop);
      g_source_remove (limit);
    }
  char *error = w.finished ? w.error : g_strdup ("timed out");
  g_main_loop_unref (w.loop);
  return error;
}

static void
discover_done (const char *error, GPtrArray *cals, gpointer data)
{
  Wait *w = data;
  w->error = g_strdup (error);
  w->cals = g_ptr_array_new_with_free_func (NULL);
  for (guint i = 0; i < cals->len; i++)
    {
      RemoteCalendar *src = cals->pdata[i], *c = g_new0 (RemoteCalendar, 1);
      c->href = g_strdup (src->href);
      c->name = g_strdup (src->name);
      c->color = g_strdup (src->color);
      g_ptr_array_add (w->cals, c);
    }
  w->finished = TRUE;
  g_main_loop_quit (w->loop);
}

static GPtrArray *
discover (CalAccount *a, char **error)
{
  Wait w = { g_main_loop_new (NULL, FALSE), NULL, NULL, FALSE };
  caldav_discover (a, discover_done, &w);
  if (!w.finished)
    {
      guint limit = g_timeout_add_seconds (60, on_timeout, w.loop);
      g_main_loop_run (w.loop);
      g_source_remove (limit);
    }
  g_main_loop_unref (w.loop);
  *error = w.finished ? w.error : g_strdup ("timed out");
  return w.cals;
}

static void
free_remotes (GPtrArray *list)
{
  for (guint i = 0; list && i < list->len; i++)
    {
      RemoteCalendar *c = list->pdata[i];
      g_free (c->href);
      g_free (c->name);
      g_free (c->color);
      g_free (c);
    }
  if (list)
    g_ptr_array_unref (list);
}

static guint
count_titled (const char *title, int y1, int m1, int d1, int y2, int m2, int d2)
{
  GArray *occ = calendar_occurrences (calendar_default (), at (y1, m1, d1, 0, 0), at (y2, m2, d2, 0, 0));
  guint n = 0;
  for (guint i = 0; i < occ->len; i++)
    n += g_str_equal (g_array_index (occ, CalOccurrence, i).event->title, title);
  g_array_free (occ, TRUE);
  return n;
}

static const CalEvent *
find_title (const char *title)
{
  const GPtrArray *all = calendar_all_events (calendar_default ());
  for (guint i = 0; i < all->len; i++)
    if (g_str_equal (((CalEvent *) all->pdata[i])->title, title))
      return all->pdata[i];
  return NULL;
}

/* "switch device": the calendar and the accounts come from another pair of files */
static void
become (const char *dir)
{
  calendar_default_free ();
  caldav_accounts_free ();
  g_autofree char *data = g_build_filename (dir, "calendar.json", NULL);
  g_autofree char *accounts = g_build_filename (dir, "accounts.json", NULL);
  g_setenv ("CALENDAR_DATA_FILE", data, TRUE);
  g_setenv ("CALENDAR_ACCOUNTS_FILE", accounts, TRUE);
}

int
main (void)
{
  /* iCloud wraps each event in CDATA, exactly like this (captured structure, invented event) */
  {
    const char *icloud_answer =
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<multistatus xmlns=\"DAV:\">\n<response xmlns=\"DAV:\">\n"
      "<href>/1/calendars/work/ABC.ics</href><propstat><prop><getetag xmlns=\"DAV:\">\"m727er5a\"</getetag>\n"
      "<calendar-data xmlns=\"urn:ietf:params:xml:ns:caldav\"><![CDATA[BEGIN:VCALENDAR\r\nSUMMARY:Tom & Jerry <party>\r\nEND:VCALENDAR\r\n]]></calendar-data>\n"
      "</prop><status>HTTP/1.1 200 OK</status></propstat></response></multistatus>";
    g_autofree char *data = caldav_xml_first_text (icloud_answer, "calendar-data");
    CHECK (data && strstr (data, "BEGIN:VCALENDAR") && strstr (data, "Tom & Jerry <party>") && strstr (data, "END:VCALENDAR"),
           "the event inside CDATA is read (%s)", data ? data : "nothing");
    CHECK (data && strstr (data, "BEGIN:VCALENDAR") < strstr (data, "SUMMARY") && strchr (data, '\n'), "with its lines kept apart");
    g_autofree char *tag = caldav_xml_first_text (icloud_answer, "getetag");
    CHECK (tag && strstr (tag, "m727er5a"), "and the tag next to it");
    /* prefixes, as other servers write them */
    g_autofree char *prefixed = caldav_xml_first_text ("<D:multistatus xmlns:D=\"DAV:\" xmlns:C=\"urn:ietf:params:xml:ns:caldav\"><D:response><C:calendar-data>X &amp; Y</C:calendar-data></D:response></D:multistatus>", "calendar-data");
    CHECK (prefixed && g_str_equal (prefixed, "X & Y"), "prefixed names and entities");
    g_autofree char *broken = caldav_xml_first_text ("<a><b>text", "b");
    CHECK (broken == NULL, "broken XML is refused");
    g_autofree char *open_cdata = caldav_xml_first_text ("<a><b><![CDATA[never closed</b></a>", "b");
    CHECK (open_cdata == NULL, "an unclosed CDATA is refused, not trusted");
  }

  /* Apple's app passwords, as Apple shows them (also pasted without dashes or with spaces) */
  CHECK (caldav_looks_like_app_password ("abcd-efgh-ijkl-mnop"), "app password with dashes");
  CHECK (caldav_looks_like_app_password ("abcdefghijklmnop"), "app password without dashes");
  CHECK (caldav_looks_like_app_password ("abcd efgh ijkl mnop"), "app password with spaces");
  CHECK (caldav_looks_like_app_password ("ABCD-EFGH-IJKL-MNOP"), "in capitals");
  CHECK (!caldav_looks_like_app_password ("MyRealPassword123"), "a normal password is not one");
  CHECK (!caldav_looks_like_app_password ("Abcd-efgh-ijkl"), "too short");
  CHECK (!caldav_looks_like_app_password ("abcd-efgh-ijkl-mnop-qrst"), "too long");
  CHECK (!caldav_looks_like_app_password (""), "empty");
  CHECK (!caldav_looks_like_app_password (NULL), "null");
  CHECK (caldav_is_icloud ("https://caldav.icloud.com"), "iCloud is recognised");
  CHECK (caldav_is_icloud ("caldav.icloud.com"), "even without the scheme");
  CHECK (caldav_is_icloud ("https://p33-caldav.icloud.com:443/"), "and its numbered hosts");
  CHECK (!caldav_is_icloud ("https://nextcloud.example.org"), "other servers are not");
  CHECK (!caldav_is_icloud ("https://evil-icloud.com"), "lookalikes are not");
  CHECK (!caldav_is_icloud (NULL), "null");
  if (failures)
    {
      g_print ("test-caldav: %d failure(s)\n", failures);
      return 1;
    }
  const char *url = g_getenv ("CALDAV_URL");
  if (!url)
    {
      g_print ("test-caldav: skipped (no CALDAV_URL)\n");
      return 0;
    }
  g_autofree char *run_id = g_uuid_string_random ();
  g_autofree char *user = g_strdup_printf ("u%.8s", run_id);
  g_autofree char *tmp_a = g_dir_make_tmp ("caldav-a-XXXXXX", NULL);
  g_autofree char *tmp_b = g_dir_make_tmp ("caldav-b-XXXXXX", NULL);
  g_autofree char *base = caldav_check_server_url (url);
  CHECK (base != NULL, "the test server address is accepted");
  CHECK (caldav_check_server_url ("http://example.org") == NULL, "plain http to another host is refused");
  g_autofree char *icloud = caldav_check_server_url ("caldav.icloud.com");
  CHECK (icloud && g_str_equal (icloud, "https://caldav.icloud.com"), "a bare host becomes https");

  /* the server needs a calendar to exist: make one with a plain request, the way a new account would */
  g_autofree char *mk = g_strdup_printf ("curl -s -o /dev/null -u %s:secret -X MKCALENDAR -H 'Content-Type: application/xml' --data '<c:mkcalendar xmlns:c=\"urn:ietf:params:xml:ns:caldav\" xmlns:d=\"DAV:\"><d:set><d:prop><d:displayname>Shared</d:displayname></d:prop></d:set></c:mkcalendar>' %s/%s/shared/", user, base, user);
  g_assert_cmpint (system (mk), ==, 0);

  /* ---- device A: sign in, find the calendar, link it, add events ---- */
  become (tmp_a);
  CalAccount *a = caldav_account_add ("Test", base, user, "secret");
  g_autofree char *account_a_id = g_strdup (a->id);
  g_autofree char *err = NULL;
  GPtrArray *cals = discover (a, &err);
  CHECK (!err && cals && cals->len >= 1, "discovery finds the calendar (%s)", err ? err : "ok");
  RemoteCalendar *rc = NULL;
  for (guint i = 0; cals && i < cals->len; i++)
    if (g_str_equal (((RemoteCalendar *) cals->pdata[i])->name, "Shared"))
      rc = cals->pdata[i];
  CHECK (rc != NULL, "it is called Shared");
  if (!rc)
    return 1;
  g_autofree char *local_a = g_strdup (caldav_link_calendar (a, rc));
  free_remotes (cals);

  CalEvent *e1 = calendar_event_new ("Dentist", at (2026, 10, 20, 15, 0), at (2026, 10, 20, 16, 0), FALSE);
  e1->calendar = g_strdup (local_a);
  g_free (e1->location);
  e1->location = g_strdup ("Clinic 4");
  calendar_event_add_alert (e1, 30);
  calendar_add (calendar_default (), e1);
  CalEvent *e2 = calendar_event_new ("Gym", at (2026, 10, 5, 7, 0), at (2026, 10, 5, 8, 0), FALSE);
  e2->calendar = g_strdup (local_a);
  e2->repeat = CAL_REPEAT_WEEKLY;
  e2->weekdays = 1 | 4;
  g_autofree char *gym_id = g_strdup (calendar_add (calendar_default (), e2));
  CalEvent *e3 = calendar_event_new ("Holiday", at (2026, 12, 24, 0, 0), at (2026, 12, 27, 0, 0), TRUE);
  e3->calendar = g_strdup (local_a);
  calendar_add (calendar_default (), e3);
  CalEvent *e4 = calendar_event_new ("Pay rent", at (2026, 10, 9, 10, 0), 0, FALSE);
  e4->calendar = g_strdup (local_a);
  e4->reminder = TRUE;
  calendar_add (calendar_default (), e4);

  g_autofree char *e_a1 = sync_and_wait ();
  CHECK (!e_a1, "first sync from A: %s", e_a1 ? e_a1 : "ok");
  CHECK (!find_title ("Dentist")->dirty && find_title ("Dentist")->href != NULL && find_title ("Dentist")->etag != NULL,
         "A's events are clean and know where they live");

  /* ---- device B: sign in to the same account and receive everything ---- */
  become (tmp_b);
  CalAccount *b = caldav_account_add ("Test", base, user, "secret");
  g_autofree char *e_b0 = NULL;
  GPtrArray *cals_b = discover (b, &e_b0);
  CHECK (!e_b0 && cals_b && cals_b->len >= 1, "B discovers it too");
  RemoteCalendar *rb = NULL;
  for (guint i = 0; cals_b && i < cals_b->len; i++)
    if (g_str_equal (((RemoteCalendar *) cals_b->pdata[i])->name, "Shared"))
      rb = cals_b->pdata[i];
  g_autofree char *local_b = g_strdup (caldav_link_calendar (b, rb));
  free_remotes (cals_b);
  g_autofree char *e_b1 = sync_and_wait ();
  CHECK (!e_b1, "B's first sync: %s", e_b1 ? e_b1 : "ok");
  const CalEvent *d = find_title ("Dentist");
  CHECK (d != NULL && d->start == at (2026, 10, 20, 15, 0) && d->end == at (2026, 10, 20, 16, 0), "B has the dentist at the right time");
  CHECK (d && g_str_equal (d->location, "Clinic 4") && calendar_event_alert_count (d) == 1 && calendar_event_alert (d, 0) == 30,
         "with its place and alert");
  const CalEvent *g = find_title ("Gym");
  CHECK (g && g->repeat == CAL_REPEAT_WEEKLY && g->weekdays == 5, "and the gym series with its weekdays");
  CHECK (count_titled ("Gym", 2026, 10, 12, 2026, 10, 19) == 2, "B sees the gym on Monday and Wednesday");
  const CalEvent *h = find_title ("Holiday");
  CHECK (h && h->all_day && h->end == at (2026, 12, 27, 0, 0), "the multi-day all-day event arrives whole");
  const CalEvent *r = find_title ("Pay rent");
  CHECK (r && r->reminder, "the reminder arrives as a reminder");

  /* ---- B edits, moves one gym showing, deletes the holiday, adds an event ---- */
  CalEvent *edit = calendar_event_new ("Dentist (moved)", at (2026, 10, 21, 15, 0), at (2026, 10, 21, 16, 0), FALSE);
  edit->id = g_strdup (find_title ("Dentist")->id);
  g_assert_true (calendar_update (calendar_default (), edit));
  CalEvent *one = calendar_event_new ("Gym (late)", at (2026, 10, 12, 9, 0), at (2026, 10, 12, 10, 0), FALSE);
  g_assert_true (calendar_apply_edit (calendar_default (), find_title ("Gym")->id, at (2026, 10, 12, 7, 0), CAL_SCOPE_ONE, one));
  g_assert_true (calendar_remove (calendar_default (), find_title ("Holiday")->id));
  CalEvent *e5 = calendar_event_new ("From B", at (2026, 10, 22, 11, 0), 0, FALSE);
  e5->calendar = g_strdup (local_b);
  calendar_add (calendar_default (), e5);
  g_autofree char *e_b2 = sync_and_wait ();
  CHECK (!e_b2, "B pushes its changes: %s", e_b2 ? e_b2 : "ok");
  CHECK (!find_title ("From B")->dirty, "B's new event is clean");

  /* ---- A receives all of it ---- */
  become (tmp_a);
  g_autofree char *e_a2 = sync_and_wait ();
  CHECK (!e_a2, "A pulls: %s", e_a2 ? e_a2 : "ok");
  CHECK (find_title ("Dentist") == NULL && find_title ("Dentist (moved)") != NULL, "A sees the renamed dentist");
  CHECK (find_title ("Dentist (moved)") && find_title ("Dentist (moved)")->start == at (2026, 10, 21, 15, 0), "at its new time");
  CHECK (find_title ("Holiday") == NULL, "the deleted holiday is gone from A");
  CHECK (find_title ("From B") != NULL, "B's new event reached A");
  CHECK (count_titled ("Gym", 2026, 10, 12, 2026, 10, 13) == 0 && count_titled ("Gym (late)", 2026, 10, 12, 2026, 10, 13) == 1,
         "the moved showing moved on A too");
  CHECK (count_titled ("Gym", 2026, 10, 14, 2026, 10, 15) == 1, "and the rest of the series stayed");

  /* ---- a conflict: both change the same event; whoever syncs second loses to the server ---- */
  CalEvent *ca = calendar_event_new ("From B (A's change)", at (2026, 10, 22, 11, 0), 0, FALSE);
  ca->id = g_strdup (find_title ("From B")->id);
  g_assert_true (calendar_update (calendar_default (), ca));
  g_autofree char *e_a3 = sync_and_wait ();
  CHECK (!e_a3, "A pushes: %s", e_a3 ? e_a3 : "ok");
  become (tmp_b);
  CalEvent *cb = calendar_event_new ("From B (B's change)", at (2026, 10, 22, 11, 0), 0, FALSE);
  cb->id = g_strdup (find_title ("From B")->id);
  g_assert_true (calendar_update (calendar_default (), cb));
  g_autofree char *e_b3 = sync_and_wait ();
  CHECK (!e_b3, "B syncs with a conflict without failing: %s", e_b3 ? e_b3 : "ok");
  CHECK (find_title ("From B (A's change)") != NULL && find_title ("From B (B's change)") == NULL, "the server's version won on B");
  CHECK (!find_title ("From B (A's change)")->dirty, "and nothing is left to send");

  /* ---- delete a whole series from B ---- */
  g_assert_true (calendar_remove (calendar_default (), find_title ("Gym")->id));
  g_autofree char *e_b4 = sync_and_wait ();
  CHECK (!e_b4, "B deletes the gym: %s", e_b4 ? e_b4 : "ok");
  become (tmp_a);
  g_autofree char *e_a4 = sync_and_wait ();
  CHECK (!e_a4, "A: %s", e_a4 ? e_a4 : "ok");
  CHECK (find_title ("Gym") == NULL && find_title ("Gym (late)") == NULL, "the whole series, with its changed showing, is gone from A");

  /* ---- a wrong password is reported, not hidden ---- */
  /* (Radicale in this test accepts anything; the message mapping is covered by the status text.) */

  /* ---- stopping the sync keeps the events ---- */
  guint before = calendar_count (calendar_default ());
  caldav_account_remove (account_a_id, FALSE);
  CHECK (calendar_count (calendar_default ()) == before, "removing the account keeps the events here");
  CHECK (calendar_calendar_find (calendar_default (), local_a)->account == NULL, "as a local calendar");

  caldav_sync_stop ();
  calendar_default_free ();
  g_print (failures ? "test-caldav: %d failure(s)\n" : "test-caldav: all checks passed\n", failures);
  return failures ? 1 : 0;
}
