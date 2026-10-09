#include <glib.h>
#include <glib/gstdio.h>

#include "../src/calendar-alerts.h"
#include "../src/calendar.h"

static gint64
at (int y, int mo, int d, int h, int mi)
{
  g_autoptr (GDateTime) dt = g_date_time_new_local (y, mo, d, h, mi, 0);
  return g_date_time_to_unix (dt);
}

/* The module has no application here, so nothing is shown, but what is due is still counted. */
static void
test_poll_and_wake (void)
{
  g_autofree char *tmp = g_dir_make_tmp ("alerts-XXXXXX", NULL);
  g_setenv ("XDG_DATA_HOME", tmp, TRUE);
  calendar_alerts_start (NULL);   /* no application: counts what is due, shows nothing */
  gint64 now = g_get_real_time () / G_USEC_PER_SEC;

  /* nothing scheduled: it sleeps an hour at most */
  g_assert_cmpint (calendar_alerts_next_wake (now), ==, now + 3600);
  g_assert_cmpuint (calendar_alerts_poll (now), ==, 0);

  /* an alert in 30 minutes: wake exactly then */
  CalEvent *e = calendar_event_new ("Soon", now + 40 * 60, 0, FALSE);
  calendar_event_add_alert (e, 10);
  g_autofree char *id = g_strdup (calendar_add (calendar_default (), e));
  g_assert_cmpint (calendar_alerts_next_wake (now), ==, now + 30 * 60);

  /* an alert more than an hour away does not make it sleep longer than an hour */
  CalEvent *far = calendar_event_new ("Far", now + 5 * 3600, 0, FALSE);
  calendar_event_add_alert (far, 0);
  calendar_add (calendar_default (), far);
  calendar_remove (calendar_default (), id);
  g_assert_cmpint (calendar_alerts_next_wake (now), ==, now + 3600);

  /* it is due: poll reports it once and never again */
  CalEvent *due = calendar_event_new ("Now", now + 120, 0, FALSE);
  calendar_event_add_alert (due, 5);
  g_autofree char *due_id = g_strdup (calendar_add (calendar_default (), due));
  g_assert_cmpuint (calendar_alerts_poll (now + 10), ==, 0);   /* not yet: it rang 3 minutes ago in real life, but alerts before start-up are skipped */
  g_assert_cmpuint (calendar_alerts_poll (now + 10), ==, 0);

  /* a snooze comes back after its time, once */
  calendar_alerts_snooze (due_id, now + 120, 540);
  g_assert_cmpint (calendar_alerts_next_wake (now), <=, now + 541);
  g_assert_cmpuint (calendar_alerts_poll (now + 100), ==, 0);
  g_assert_cmpuint (calendar_alerts_poll (now + 600), ==, 1);
  g_assert_cmpuint (calendar_alerts_poll (now + 601), ==, 0);
  /* a snooze for an event that was deleted meanwhile just disappears */
  calendar_alerts_snooze (due_id, now + 120, 5);
  calendar_remove (calendar_default (), due_id);
  /* only the 'Far' event's own alert is left to ring by then; the snooze of the deleted one is gone */
  g_assert_cmpuint (calendar_alerts_poll (now + 100000), ==, 1);
  g_assert_cmpuint (calendar_alerts_poll (now + 100001), ==, 0);

  calendar_alerts_stop ();
  calendar_default_free ();
  g_autofree char *file = g_build_filename (tmp, "npu-chat", "calendar.json", NULL);
  g_autofree char *dir = g_build_filename (tmp, "npu-chat", NULL);
  g_remove (file);
  g_rmdir (dir);
  g_rmdir (tmp);
  (void) at;
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/alerts/poll-and-wake", test_poll_and_wake);
  return g_test_run ();
}
