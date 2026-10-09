#include "calendar-subscribe.h"

#include <string.h>

#include "calendar-ics.h"
#include "calendar.h"
#include "net.h"

#define MAX_BYTES (8 * 1024 * 1024)
#define CHECK_EVERY 3600   /* seconds between looks at which subscriptions are due */

static const HolidayRegion regions[] = {
  { "Alemania", "Germany", "de.german" },
  { "Argentina", "Argentina", "es.ar" },
  { "Australia", "Australia", "en.australian" },
  { "Bolivia", "Bolivia", "es.bo" },
  { "Brasil", "Brazil", "pt.brazilian" },
  { "Canadá", "Canada", "en.canadian" },
  { "Chile", "Chile", "es.cl" },
  { "Colombia", "Colombia", "es.co" },
  { "Costa Rica", "Costa Rica", "es.cr" },
  { "Cuba", "Cuba", "es.cu" },
  { "Ecuador", "Ecuador", "es.ec" },
  { "El Salvador", "El Salvador", "es.sv" },
  { "España", "Spain", "es.spain" },
  { "Estados Unidos", "United States", "en.usa" },
  { "Francia", "France", "fr.french" },
  { "Guatemala", "Guatemala", "es.gt" },
  { "Honduras", "Honduras", "es.hn" },
  { "India", "India", "en.indian" },
  { "Italia", "Italy", "it.italian" },
  { "Japón", "Japan", "ja.japanese" },
  { "México", "Mexico", "es.mexican" },
  { "Nicaragua", "Nicaragua", "es.ni" },
  { "Países Bajos", "Netherlands", "nl.dutch" },
  { "Panamá", "Panama", "es.pa" },
  { "Paraguay", "Paraguay", "es.py" },
  { "Perú", "Peru", "es.pe" },
  { "Portugal", "Portugal", "pt.portuguese" },
  { "Puerto Rico", "Puerto Rico", "es.pr" },
  { "Reino Unido", "United Kingdom", "en.uk" },
  { "República Dominicana", "Dominican Republic", "es.do" },
  { "Uruguay", "Uruguay", "es.uy" },
  { "Venezuela", "Venezuela", "es.ve" },
};

const HolidayRegion *
calendar_holiday_regions (guint *count)
{
  *count = G_N_ELEMENTS (regions);
  return regions;
}

char *
calendar_holiday_url (const HolidayRegion *region)
{
  return g_strdup_printf ("https://calendar.google.com/calendar/ical/%s%%23holiday%%40group.v.calendar.google.com/public/basic.ics",
                          region->code);
}

char *
calendar_subscription_normalize_url (const char *text)
{
  if (!text)
    return NULL;
  g_autofree char *t = g_strstrip (g_strdup (text));
  if (g_ascii_strncasecmp (t, "webcal://", 9) == 0)
    return g_strconcat ("https://", t + 9, NULL);
  if (g_ascii_strncasecmp (t, "webcals://", 10) == 0)
    return g_strconcat ("https://", t + 10, NULL);
  if (g_ascii_strncasecmp (t, "https://", 8) == 0 || g_ascii_strncasecmp (t, "http://", 7) == 0)
    {
      g_autoptr (GUri) uri = g_uri_parse (t, G_URI_FLAGS_PARSE_RELAXED, NULL);
      return uri && g_uri_get_host (uri) && *g_uri_get_host (uri) ? g_steal_pointer (&t) : NULL;
    }
  return NULL;
}

/* ---- fetching ----------------------------------------------------------- */

typedef struct {
  char          *calendar_id;
  SubscribeDone  done;
  gpointer       data;
  GCancellable  *cancel;
  SoupMessage   *msg;      /* kept to read the HTTP status once the body arrives */
} Fetch;

static GCancellable *cancel_all;
static void (*changed_cb) (void);
static guint timer;
static guint pending;

static void
fetch_finish (Fetch *f, guint events, const char *error)
{
  f->done (f->calendar_id, events, error, f->data);
  g_free (f->calendar_id);
  g_object_unref (f->cancel);
  g_clear_object (&f->msg);
  g_free (f);
}

static void
on_body (GObject *source, GAsyncResult *res, gpointer user_data)
{
  Fetch *f = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) body = soup_session_send_and_read_finish (SOUP_SESSION (source), res, &error);
  if (!body)
    {
      fetch_finish (f, 0, error->message);
      return;
    }
  guint status = soup_message_get_status (f->msg);
  if (status < 200 || status >= 300)
    {
      g_autofree char *why = g_strdup_printf ("The server answered with error %u.", status);
      fetch_finish (f, 0, why);
      return;
    }
  if (g_bytes_get_size (body) == 0 || !g_bytes_get_data (body, NULL))
    {
      fetch_finish (f, 0, "The server sent nothing.");
      return;
    }
  if (g_bytes_get_size (body) > MAX_BYTES)
    {
      fetch_finish (f, 0, "The calendar is too large.");
      return;
    }
  g_autofree char *text = g_strndup (g_bytes_get_data (body, NULL), g_bytes_get_size (body));
  if (!strstr (text, "BEGIN:VCALENDAR"))
    {
      fetch_finish (f, 0, "That address is not a calendar.");
      return;
    }
  GPtrArray *events = calendar_ics_parse (text);
  guint n = events->len;
  CalCalendar *c = calendar_calendar_find (calendar_default (), f->calendar_id);
  if (c && g_str_equal (c->id, f->calendar_id) && c->url)
    {
      calendar_replace_events (calendar_default (), f->calendar_id, events);
      if (changed_cb)
        changed_cb ();
      g_ptr_array_unref (events);
      fetch_finish (f, n, NULL);
      return;
    }
  g_ptr_array_unref (events);
  fetch_finish (f, 0, "The calendar was removed.");
}

void
calendar_subscription_fetch (const char *calendar_id, SubscribeDone done, gpointer data)
{
  Fetch *f = g_new0 (Fetch, 1);
  f->calendar_id = g_strdup (calendar_id);
  f->done = done;
  f->data = data;
  if (!cancel_all)
    cancel_all = g_cancellable_new ();
  f->cancel = g_object_ref (cancel_all);
  CalCalendar *c = calendar_calendar_find (calendar_default (), calendar_id);
  g_autofree char *url = c && c->url ? calendar_subscription_normalize_url (c->url) : NULL;
  if (!url || !g_str_equal (c->id, calendar_id))
    {
      fetch_finish (f, 0, "This calendar has no address.");
      return;
    }
  f->msg = soup_message_new (SOUP_METHOD_GET, url);
  if (!f->msg)
    {
      fetch_finish (f, 0, "This calendar has no valid address.");
      return;
    }
  soup_message_headers_append (soup_message_get_request_headers (f->msg), "Accept", "text/calendar, */*;q=0.5");
  soup_session_send_and_read_async (net_session (), f->msg, G_PRIORITY_DEFAULT, f->cancel, on_body, f);
}

/* ---- the schedule ------------------------------------------------------- */

static void
scheduled_done (const char *calendar_id, guint events, const char *error, gpointer data)
{
  (void) calendar_id; (void) events; (void) data;
  if (error)
    g_message ("calendar: refreshing a subscription failed: %s", error);
  if (pending)
    pending--;
}

static void
refresh_due (gboolean all)
{
  gint64 now = g_get_real_time () / G_USEC_PER_SEC;
  GPtrArray *cals = calendar_calendars (calendar_default ());
  /* collect first: a refresh may change the list while we walk it */
  g_autoptr (GPtrArray) due = g_ptr_array_new_with_free_func (g_free);
  for (guint i = 0; i < cals->len; i++)
    {
      const CalCalendar *c = cals->pdata[i];
      if (c->url && (all || now - c->fetched >= (gint64) MAX (c->refresh_hours, 1) * 3600))
        g_ptr_array_add (due, g_strdup (c->id));
    }
  for (guint i = 0; i < due->len; i++)
    {
      pending++;
      calendar_subscription_fetch (due->pdata[i], scheduled_done, NULL);
    }
}

static gboolean
on_timer (gpointer data)
{
  (void) data;
  refresh_due (FALSE);
  return G_SOURCE_CONTINUE;
}

static gboolean
first_look (gpointer data)
{
  (void) data;
  refresh_due (FALSE);
  timer = g_timeout_add_seconds (CHECK_EVERY, on_timer, NULL);
  return G_SOURCE_REMOVE;
}

void
calendar_subscriptions_start (void (*changed) (void))
{
  changed_cb = changed;
  if (!timer)
    timer = g_timeout_add_seconds (5, first_look, NULL);   /* let the window appear first */
}

void
calendar_subscriptions_stop (void)
{
  if (timer)
    g_source_remove (timer);
  timer = 0;
  if (cancel_all)
    g_cancellable_cancel (cancel_all);
  /* let the cancelled requests report back before the session goes away */
  while (pending && g_main_context_iteration (NULL, FALSE))
    ;
  g_clear_object (&cancel_all);
  changed_cb = NULL;
  net_shutdown ();
}

void
calendar_subscriptions_refresh_all (void)
{
  refresh_due (TRUE);
}
