#include "calendar-alerts.h"

#include "calendar.h"
#include "i18n.h"

#define MAX_WAIT 3600   /* seconds: wake at least this often to notice clock jumps and suspend */

static GApplication *app;
static guint timer;
static gint64 last_fire;   /* alerts at or before this moment were already handled */

static void arm (void);

static char *
body_for (const CalAlert *a)
{
  g_autoptr (GDateTime) d = g_date_time_new_from_unix_local (a->start);
  g_autofree char *when = a->event->all_day ? g_strdup (TR ("Todo el día", "All day")) : g_date_time_format (d, "%H:%M");
  if (*a->event->location)
    return g_strdup_printf ("%s · %s", when, a->event->location);
  return g_strdup (when);
}

static gboolean
on_timer (gpointer data)
{
  (void) data;
  timer = 0;
  gint64 now = g_get_real_time () / G_USEC_PER_SEC;
  CalAlert a;
  /* everything due by now, in order; usually one, several after a suspend */
  while (calendar_next_alert (calendar_default (), last_fire, &a) && a.fire <= now)
    {
      last_fire = a.fire;
      if (app)
        {
          g_autofree char *body = body_for (&a);
          g_autoptr (GNotification) n = g_notification_new (a.event->title);
          g_notification_set_body (n, body);
          g_notification_set_priority (n, G_NOTIFICATION_PRIORITY_HIGH);
          g_autofree char *id = g_strdup_printf ("%s-%" G_GINT64_FORMAT, a.event->id, a.start);
          g_application_send_notification (app, id, n);
        }
    }
  arm ();
  return G_SOURCE_REMOVE;
}

static void
arm (void)
{
  if (timer)
    {
      g_source_remove (timer);
      timer = 0;
    }
  if (!app)
    return;
  gint64 now = g_get_real_time () / G_USEC_PER_SEC;
  CalAlert a;
  gint64 wait = MAX_WAIT;
  if (calendar_next_alert (calendar_default (), last_fire, &a))
    wait = CLAMP (a.fire - now, 1, MAX_WAIT);
  timer = g_timeout_add_seconds ((guint) wait, on_timer, NULL);
}

void
calendar_alerts_start (GApplication *application)
{
  app = application;
  /* alerts that passed while the app was closed are not replayed */
  last_fire = g_get_real_time () / G_USEC_PER_SEC;
  arm ();
}

void
calendar_alerts_reschedule (void)
{
  arm ();
}

void
calendar_alerts_stop (void)
{
  if (timer)
    g_source_remove (timer);
  timer = 0;
  app = NULL;
}
