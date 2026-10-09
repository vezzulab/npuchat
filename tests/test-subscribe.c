#include <glib.h>
#include <glib/gstdio.h>
#include <libsoup/soup.h>

#include "../src/calendar-subscribe.h"
#include "../src/calendar.h"
#include "../src/net.h"

/* NPU Chat builds net.c with an object tracker that tests do not have */
#ifdef NPU_CHAT_SELFTEST
gpointer selftest_track (gpointer object, const char *what) { (void) what; return object; }
#endif

static const char *ics =
  "BEGIN:VCALENDAR\r\nVERSION:2.0\r\n"
  "BEGIN:VEVENT\r\nUID:h1\r\nDTSTART;VALUE=DATE:20261112\r\nDTEND;VALUE=DATE:20261113\r\nSUMMARY:Holiday one\r\nEND:VEVENT\r\n"
  "BEGIN:VEVENT\r\nUID:h2\r\nDTSTART;VALUE=DATE:20261225\r\nDTEND;VALUE=DATE:20261226\r\nSUMMARY:Holiday two\r\nEND:VEVENT\r\n"
  "END:VCALENDAR\r\n";
static const char *ics_one =
  "BEGIN:VCALENDAR\r\nVERSION:2.0\r\n"
  "BEGIN:VEVENT\r\nUID:h1\r\nDTSTART;VALUE=DATE:20261112\r\nDTEND;VALUE=DATE:20261113\r\nSUMMARY:Holiday one\r\nEND:VEVENT\r\n"
  "END:VCALENDAR\r\n";

static const char *served = NULL;   /* what the test server answers with; NULL gives an error */

static void
handler (SoupServer *server, SoupServerMessage *msg, const char *path, GHashTable *query, gpointer data)
{
  (void) server; (void) path; (void) query; (void) data;
  if (!served)
    {
      soup_server_message_set_status (msg, 500, NULL);
      return;
    }
  soup_server_message_set_status (msg, 200, NULL);
  soup_server_message_set_response (msg, "text/calendar", SOUP_MEMORY_COPY, served, strlen (served));
}

typedef struct {
  GMainLoop *loop;
  char      *error;
  guint      events;
  gboolean   finished;
} Result;

static void
done (const char *id, guint events, const char *error, gpointer data)
{
  (void) id;
  Result *r = data;
  r->events = events;
  r->error = g_strdup (error);
  r->finished = TRUE;
  g_main_loop_quit (r->loop);
}

static Result
fetch (const char *id)
{
  Result r = { g_main_loop_new (NULL, FALSE), NULL, 0, FALSE };
  calendar_subscription_fetch (id, done, &r);
  if (!r.finished)       /* an answer that needs no network comes back at once */
    g_main_loop_run (r.loop);
  g_main_loop_unref (r.loop);
  return r;
}

static guint
count_in (int y1, int m1, int y2, int m2)
{
  g_autoptr (GDateTime) a = g_date_time_new_local (y1, m1, 1, 0, 0, 0);
  g_autoptr (GDateTime) b = g_date_time_new_local (y2, m2, 1, 0, 0, 0);
  GArray *occ = calendar_occurrences (calendar_default (), g_date_time_to_unix (a), g_date_time_to_unix (b));
  guint n = occ->len;
  g_array_free (occ, TRUE);
  return n;
}

static void
test_normalize (void)
{
  g_autofree char *a = calendar_subscription_normalize_url ("webcal://example.org/a.ics");
  g_assert_cmpstr (a, ==, "https://example.org/a.ics");
  g_autofree char *b = calendar_subscription_normalize_url ("  https://example.org/x  ");
  g_assert_cmpstr (b, ==, "https://example.org/x");
  g_autofree char *c = calendar_subscription_normalize_url ("http://127.0.0.1:8080/c.ics");
  g_assert_cmpstr (c, ==, "http://127.0.0.1:8080/c.ics");
  g_assert_null (calendar_subscription_normalize_url ("ftp://example.org/a.ics"));
  g_assert_null (calendar_subscription_normalize_url ("file:///etc/passwd"));
  g_assert_null (calendar_subscription_normalize_url ("javascript:alert(1)"));
  g_assert_null (calendar_subscription_normalize_url ("https://"));
  g_assert_null (calendar_subscription_normalize_url (""));
  g_assert_null (calendar_subscription_normalize_url (NULL));
  guint n;
  const HolidayRegion *r = calendar_holiday_regions (&n);
  g_assert_cmpuint (n, >, 20);
  g_autofree char *url = calendar_holiday_url (&r[0]);
  g_assert_true (g_str_has_prefix (url, "https://calendar.google.com/calendar/ical/"));
  g_assert_nonnull (strstr (url, "%23holiday%40group"));
}

static void
test_fetch (void)
{
  g_autofree char *tmp = g_dir_make_tmp ("sub-XXXXXX", NULL);
  g_setenv ("XDG_DATA_HOME", tmp, TRUE);

  g_autoptr (SoupServer) server = soup_server_new (NULL, NULL);
  soup_server_add_handler (server, "/", handler, NULL, NULL);
  g_autoptr (GError) error = NULL;
  g_assert_true (soup_server_listen_local (server, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, &error));
  GSList *uris = soup_server_get_uris (server);
  g_autofree char *url = g_uri_to_string (uris->data);
  g_slist_free_full (uris, (GDestroyNotify) g_uri_unref);

  const char *id = calendar_subscription_add (calendar_default (), "Holidays", 4, url, 24);
  g_autofree char *sid = g_strdup (id);

  served = ics;
  Result r = fetch (sid);
  g_assert_null (r.error);
  g_assert_cmpuint (r.events, ==, 2);
  g_assert_cmpuint (count_in (2026, 11, 2027, 1), ==, 2);

  /* the answer shrinks: the old events are replaced, not piled up */
  served = ics_one;
  r = fetch (sid);
  g_assert_null (r.error);
  g_assert_cmpuint (count_in (2026, 11, 2027, 1), ==, 1);

  /* a server error leaves what was there alone, and says so */
  served = NULL;
  r = fetch (sid);
  g_assert_nonnull (r.error);
  g_free (r.error);
  g_assert_cmpuint (count_in (2026, 11, 2027, 1), ==, 1);

  /* something that is not a calendar is refused and changes nothing */
  served = "<html>hello</html>";
  r = fetch (sid);
  g_assert_nonnull (r.error);
  g_free (r.error);
  g_assert_cmpuint (count_in (2026, 11, 2027, 1), ==, 1);

  /* a calendar that was removed meanwhile */
  served = ics;
  calendar_calendar_remove (calendar_default (), sid);
  r = fetch (sid);
  g_assert_nonnull (r.error);
  g_free (r.error);

  calendar_subscriptions_stop ();
  calendar_default_free ();
  g_autofree char *file = g_build_filename (tmp, "npu-chat", "calendar.json", NULL);
  g_autofree char *dir = g_build_filename (tmp, "npu-chat", NULL);
  g_remove (file);
  g_rmdir (dir);
  g_rmdir (tmp);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/subscribe/normalize", test_normalize);
  g_test_add_func ("/subscribe/fetch", test_fetch);
  return g_test_run ();
}
