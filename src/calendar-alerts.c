#include "calendar-alerts.h"

#include "calendar.h"
#include "i18n.h"

#define MAX_WAIT 3600   /* seconds: wake at least this often to notice clock jumps and suspend */

typedef struct {
  char  *id;
  gint64 occ_start;
  gint64 fire;
} Snooze;

static GApplication *app;
static char *icon_name;
static guint timer;
static gint64 last_fire;   /* alerts at or before this moment were already handled */
static GArray *snoozes;    /* Snooze, the ones waiting to come back */

static void arm (void);

static void
snooze_clear (gpointer p)
{
  g_free (((Snooze *) p)->id);
}

static char *
body_for (const CalEvent *ev, gint64 start)
{
  g_autoptr (GDateTime) d = g_date_time_new_from_unix_local (start);
  g_autofree char *when = ev->all_day ? g_strdup (TR ("Todo el día", "All day")) : g_date_time_format (d, "%H:%M");
  if (*ev->location)
    return g_strdup_printf ("%s · %s", when, ev->location);
  return g_strdup (when);
}

static void
notify (const CalEvent *ev, gint64 start)
{
  if (!app)
    return;
  g_autofree char *body = body_for (ev, start);
  g_autoptr (GNotification) n = g_notification_new (ev->title);
  g_notification_set_body (n, body);
  g_notification_set_priority (n, G_NOTIFICATION_PRIORITY_HIGH);
  g_autoptr (GIcon) icon = g_themed_icon_new (icon_name ? icon_name : "io.github.vezzulab.Calendar");
  g_notification_set_icon (n, icon);
  g_notification_set_default_action (n, "app.calendar-open");
  g_notification_add_button_with_target (n, TR ("Posponer", "Snooze"), "app.calendar-snooze", "(sx)", ev->id, start);
  g_autofree char *id = g_strdup_printf ("%s-%" G_GINT64_FORMAT, ev->id, start);
  g_application_send_notification (app, id, n);
}

guint
calendar_alerts_poll (gint64 now)
{
  guint fired = 0;
  CalAlert a;
  /* everything due by now, in order; usually one, several after a suspend */
  while (calendar_next_alert (calendar_default (), last_fire, &a) && a.fire <= now)
    {
      last_fire = a.fire;
      notify (a.event, a.start);
      fired++;
    }
  for (guint i = snoozes ? snoozes->len : 0; i > 0; i--)
    {
      Snooze *z = &g_array_index (snoozes, Snooze, i - 1);
      if (z->fire > now)
        continue;
      const CalEvent *ev = calendar_find (calendar_default (), z->id);
      if (ev)
        {
          notify (ev, z->occ_start);
          fired++;
        }
      g_array_remove_index (snoozes, i - 1);
    }
  return fired;
}

void
calendar_alerts_snooze (const char *event_id, gint64 occurrence_start, int seconds)
{
  if (!snoozes)
    {
      snoozes = g_array_new (FALSE, FALSE, sizeof (Snooze));
      g_array_set_clear_func (snoozes, snooze_clear);
    }
  Snooze z = { g_strdup (event_id), occurrence_start, g_get_real_time () / G_USEC_PER_SEC + seconds };
  g_array_append_val (snoozes, z);
  arm ();
}

gint64
calendar_alerts_next_wake (gint64 now)
{
  gint64 wake = now + MAX_WAIT;
  CalAlert a;
  if (calendar_next_alert (calendar_default (), last_fire, &a))
    wake = MIN (wake, MAX (a.fire, now + 1));
  for (guint i = 0; snoozes && i < snoozes->len; i++)
    wake = MIN (wake, MAX (g_array_index (snoozes, Snooze, i).fire, now + 1));
  return wake;
}

static gboolean
on_timer (gpointer data)
{
  (void) data;
  timer = 0;
  calendar_alerts_poll (g_get_real_time () / G_USEC_PER_SEC);
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
  timer = g_timeout_add_seconds ((guint) (calendar_alerts_next_wake (now) - now), on_timer, NULL);
}

static void
on_snooze (GSimpleAction *action, GVariant *param, gpointer data)
{
  (void) action; (void) data;
  const char *id;
  gint64 occ;
  g_variant_get (param, "(&sx)", &id, &occ);
  calendar_alerts_snooze (id, occ, CALENDAR_SNOOZE_SECONDS);
}

static void
on_open (GSimpleAction *action, GVariant *param, gpointer data)
{
  (void) action; (void) param; (void) data;
  if (app)
    g_application_activate (app);
}

/* the other program (Calendar app / NPU Chat) may have saved: pick that up */
static guint watch;
static void (*external_change) (void);

void
calendar_alerts_set_external_change_handler (void (*handler) (void))
{
  external_change = handler;
}

static gboolean
on_watch (gpointer data)
{
  (void) data;
  if (calendar_reload_if_changed (calendar_default ()))
    {
      arm ();
      if (external_change)
        external_change ();
    }
  return G_SOURCE_CONTINUE;
}

void
calendar_alerts_set_icon (const char *name)
{
  g_free (icon_name);
  icon_name = g_strdup (name);
}

void
calendar_alerts_start (GApplication *application)
{
  app = application;
  /* alerts that passed while the app was closed are not replayed */
  last_fire = g_get_real_time () / G_USEC_PER_SEC;
  GActionEntry entries[] = {
    { "calendar-snooze", on_snooze, "(sx)", NULL, NULL, { 0 } },
    { "calendar-open", on_open, NULL, NULL, NULL, { 0 } },
  };
  if (app && !g_action_map_lookup_action (G_ACTION_MAP (app), "calendar-snooze"))
    g_action_map_add_action_entries (G_ACTION_MAP (app), entries, G_N_ELEMENTS (entries), NULL);
  arm ();
  if (!watch)
    watch = g_timeout_add_seconds (2, on_watch, NULL);
}

void
calendar_alerts_reschedule (void)
{
  arm ();
}

void
calendar_alerts_stop (void)
{
  if (watch)
    g_source_remove (watch);
  watch = 0;
  if (timer)
    g_source_remove (timer);
  timer = 0;
  app = NULL;
  g_clear_pointer (&icon_name, g_free);
  if (snoozes)
    g_array_free (snoozes, TRUE);
  snoozes = NULL;
}
