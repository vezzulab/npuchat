#include "calendar-tools.h"

#include <string.h>

#include "calendar-quick.h"
#include "calendar-settings.h"
#include "calendar.h"
#include "i18n.h"

#define RESULT_LIMIT 1300

static const char *days_es[] = { "lunes", "martes", "miércoles", "jueves", "viernes", "sábado", "domingo" };
static const char *days_en[] = { "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday" };
static const char *months_es[] = { "enero", "febrero", "marzo", "abril", "mayo", "junio", "julio", "agosto",
                                   "septiembre", "octubre", "noviembre", "diciembre" };
static const char *months_en[] = { "January", "February", "March", "April", "May", "June", "July", "August",
                                   "September", "October", "November", "December" };

static gboolean
english (void)
{
  return i18n_lang () == LANG_EN;
}

static gint64
now_unix (void)
{
  return g_get_real_time () / G_USEC_PER_SEC;
}

static gint64
shift_days (gint64 t, int n)
{
  g_autoptr (GDateTime) d = g_date_time_new_from_unix_local (t);
  g_autoptr (GDateTime) r = g_date_time_add_days (d, n);
  return g_date_time_to_unix (r);
}

static char *
fold (const char *text)
{
  g_autofree char *ascii = g_str_to_ascii (text ? text : "", NULL);
  return g_ascii_strdown (ascii, -1);
}

static const CalCalendar *
cal_of (const CalEvent *ev)
{
  return calendar_calendar_find (calendar_default (), ev->calendar);
}

/* "- Thu 8 October, 19:30–22:00: Dinner (Family) @ Home" */
static char *
line_for (const CalOccurrence *o)
{
  g_autoptr (GDateTime) d = g_date_time_new_from_unix_local (o->start);
  int dow = g_date_time_get_day_of_week (d) - 1, month = g_date_time_get_month (d) - 1;
  g_autofree char *when = NULL;
  if (o->event->all_day)
    when = g_strdup (TR ("todo el día", "all day"));
  else if (o->event->reminder)
    when = g_date_time_format (d, "%H:%M");
  else
    {
      g_autoptr (GDateTime) e = g_date_time_new_from_unix_local (o->end);
      g_autofree char *a = g_date_time_format (d, "%H:%M");
      g_autofree char *b = g_date_time_format (e, "%H:%M");
      when = g_strdup_printf ("%s–%s", a, b);
    }
  const char *mark = o->event->reminder ? (calendar_is_done (o->event, o->start) ? TR ("[hecho] ", "[done] ") : TR ("[recordatorio] ", "[reminder] ")) : "";
  return g_strdup_printf ("- %s %d %s, %s: %s%s (%s)%s%s", english () ? days_en[dow] : days_es[dow],
                          g_date_time_get_day_of_month (d), english () ? months_en[month] : months_es[month], when, mark,
                          o->event->title, cal_of (o->event)->name, *o->event->location ? " @ " : "", o->event->location);
}

/* "today", "tomorrow", "week", "next week", a date: the range it means */
static gboolean
range_of (const char *when, gint64 *from, gint64 *to)
{
  g_autofree char *trimmed = g_strstrip (g_strdup (when ? when : ""));
  g_autofree char *lower = g_ascii_strdown (trimmed, -1);
  g_autofree char *a = g_str_to_ascii (lower, NULL);
  gint64 today = calendar_day_start (now_unix ());
  gint64 t;
  gboolean date_only;
  *from = today;
  if (!*a || strstr (a, "today") || strstr (a, "hoy"))
    *to = calendar_day_next (today);
  else if (strstr (a, "tomorrow") || strstr (a, "manana"))
    {
      *from = calendar_day_next (today);
      *to = calendar_day_next (*from);
    }
  else if (strstr (a, "next week") || strstr (a, "proxima semana"))
    {
      *from = shift_days (today, 7);
      *to = shift_days (today, 14);
    }
  else if (strstr (a, "month") || strstr (a, " mes") || g_str_equal (a, "mes"))
    *to = shift_days (today, 31);
  else if (strstr (a, "week") || strstr (a, "semana"))
    *to = shift_days (today, 7);
  else if (calendar_parse_time (a, &t, &date_only))
    {
      *from = calendar_day_start (t);
      *to = calendar_day_next (*from);
    }
  else
    return FALSE;
  return TRUE;
}

/* ---- agenda ------------------------------------------------------------- */

char *
calendar_tool_agenda (const char *when)
{
  gint64 from, to;
  if (!range_of (when, &from, &to))
    return g_strdup ("Error: say today, tomorrow, week, next week, or a date like 2026-10-09.");
  GArray *occ = calendar_occurrences (calendar_default (), from, to);
  GString *out = g_string_new (NULL);
  guint n = 0;
  for (guint i = 0; i < occ->len && out->len < RESULT_LIMIT; i++)
    {
      const CalOccurrence *o = &g_array_index (occ, CalOccurrence, i);
      if (!cal_of (o->event)->visible)
        continue;
      g_autofree char *line = line_for (o);
      g_string_append_printf (out, "%s\n", line);
      n++;
    }
  g_array_free (occ, TRUE);
  if (!n)
    {
      g_string_free (out, TRUE);
      return g_strdup (TR ("No hay eventos en ese período.", "No events in that period."));
    }
  return g_string_free (out, FALSE);
}

/* ---- add ---------------------------------------------------------------- */

char *
calendar_tool_add (const char *text, gboolean *ok)
{
  CalQuick q;
  *ok = FALSE;
  if (!text || !calendar_quick_parse (text, now_unix (), english (), &q))
    return g_strdup ("Error: describe the event, for example: dentist tomorrow 3pm.");
  if (!*q.title)
    {
      calendar_quick_clear (&q);
      return g_strdup ("Error: the event needs a name, for example: dentist tomorrow 3pm.");
    }
  CalEvent *ev = calendar_quick_to_event (&q, "");
  const CalSettings *cs = calendar_settings ();
  if (cs->default_alert >= 0 && !calendar_event_alert_count (ev) && !ev->reminder)
    calendar_event_add_alert (ev, cs->default_alert);
  CalOccurrence o = { ev, q.start, q.end };
  g_autofree char *line = line_for (&o);
  calendar_add (calendar_default (), ev);
  calendar_quick_clear (&q);
  *ok = TRUE;
  return g_strdup_printf ("%s %s", TR ("Apuntado:", "Added:"), line + 2);
}

/* ---- finding an event by its title -------------------------------------- */

typedef struct {
  GPtrArray *events;    /* const CalEvent*, one per distinct event */
  GArray    *starts;    /* gint64: the showing to act on for each */
} Matches;

static void
matches_free (Matches *m)
{
  g_ptr_array_unref (m->events);
  g_array_free (m->starts, TRUE);
}

/* The upcoming (or just past) showings whose title contains the words. */
static Matches
find_matches (const char *query)
{
  Matches m = { g_ptr_array_new (), g_array_new (FALSE, FALSE, sizeof (gint64)) };
  g_autofree char *needle = fold (query);
  g_strstrip (needle);
  if (!*needle)
    return m;
  gint64 today = calendar_day_start (now_unix ());
  GArray *occ = calendar_occurrences (calendar_default (), shift_days (today, -1), shift_days (today, 180));
  for (guint i = 0; i < occ->len; i++)
    {
      const CalOccurrence *o = &g_array_index (occ, CalOccurrence, i);
      g_autofree char *hay = fold (o->event->title);
      if (!strstr (hay, needle))
        continue;
      /* a repeating event means its next showing, not one that already happened */
      if (o->event->repeat != CAL_REPEAT_NONE && o->end <= now_unix ())
        continue;
      gboolean seen = FALSE;
      for (guint k = 0; k < m.events->len; k++)
        seen |= m.events->pdata[k] == o->event;
      if (seen)
        continue;
      g_ptr_array_add (m.events, (gpointer) o->event);
      g_array_append_val (m.starts, o->start);
    }
  g_array_free (occ, TRUE);
  return m;
}

/* One event, or an explanation that the model can pass on. */
static const CalEvent *
unique_match (const char *query, Matches *m, gint64 *occ_start, char **problem)
{
  *m = find_matches (query);
  if (!m->events->len)
    {
      *problem = g_strdup_printf ("Error: no event matches \"%s\" in the next months.", query ? query : "");
      return NULL;
    }
  if (m->events->len > 1)
    {
      GString *s = g_string_new ("Error: several events match, ask which one:\n");
      for (guint i = 0; i < m->events->len && i < 6; i++)
        {
          CalOccurrence o = { m->events->pdata[i], g_array_index (m->starts, gint64, i), 0 };
          o.end = o.start + (o.event->end - o.event->start);
          g_autofree char *line = line_for (&o);
          g_string_append_printf (s, "%s\n", line);
        }
      *problem = g_string_free (s, FALSE);
      return NULL;
    }
  if (cal_of (m->events->pdata[0])->url)
    {
      *problem = g_strdup ("Error: that event belongs to a subscribed calendar, which is read-only.");
      return NULL;
    }
  *occ_start = g_array_index (m->starts, gint64, 0);
  return m->events->pdata[0];
}

char *
calendar_tool_find (const char *query)
{
  Matches m = find_matches (query);
  GString *out = g_string_new (NULL);
  for (guint i = 0; i < m.events->len && i < 10 && out->len < RESULT_LIMIT; i++)
    {
      CalOccurrence o = { m.events->pdata[i], g_array_index (m.starts, gint64, i), 0 };
      o.end = o.start + (o.event->end - o.event->start);
      g_autofree char *line = line_for (&o);
      g_string_append_printf (out, "%s\n", line);
    }
  matches_free (&m);
  if (!out->len)
    {
      g_string_free (out, TRUE);
      return g_strdup (TR ("No encontré eventos con ese nombre.", "No events with that name."));
    }
  return g_string_free (out, FALSE);
}

/* ---- change, delete, undo, done ----------------------------------------- */

char *
calendar_tool_change (const char *event, const char *new_time, const char *new_title, const char *new_location, gboolean *ok)
{
  *ok = FALSE;
  Matches m;
  gint64 occ;
  g_autofree char *problem = NULL;
  const CalEvent *ev = unique_match (event, &m, &occ, &problem);
  if (!ev)
    {
      matches_free (&m);
      return g_steal_pointer (&problem);
    }
  g_autofree char *time_text = g_strstrip (g_strdup (new_time ? new_time : ""));
  gboolean has_time = *time_text != 0;
  gboolean has_title = new_title && *new_title;
  gboolean has_place = new_location && *new_location;
  if (!has_time && !has_title && !has_place)
    {
      matches_free (&m);
      return g_strdup ("Error: say what to change: new_time, new_title or new_location.");
    }
  CalEvent *edited = calendar_event_copy (ev);
  gint64 length = ev->end - ev->start;
  edited->start = occ;
  edited->end = occ + length;
  if (has_time)
    {
      CalQuick q;
      if (!calendar_quick_parse (time_text, now_unix (), english (), &q))
        {
          calendar_event_free (edited);
          matches_free (&m);
          return g_strdup ("Error: say the new time like: friday 4pm, tomorrow, 2026-10-20 10:00.");
        }
      if (q.all_day && !ev->all_day)
        {
          /* only a day was given: the event keeps its clock time on that day */
          g_autoptr (GDateTime) d = g_date_time_new_from_unix_local (q.start);
          g_autoptr (GDateTime) o = g_date_time_new_from_unix_local (occ);
          g_autoptr (GDateTime) t = g_date_time_new_local (g_date_time_get_year (d), g_date_time_get_month (d), g_date_time_get_day_of_month (d),
                                                            g_date_time_get_hour (o), g_date_time_get_minute (o), 0);
          edited->start = g_date_time_to_unix (t);
        }
      else
        edited->start = q.start;
      edited->end = edited->start + length;
      calendar_quick_clear (&q);
    }
  if (has_title)
    {
      g_free (edited->title);
      edited->title = g_strdup (new_title);
    }
  if (has_place)
    {
      g_free (edited->location);
      edited->location = g_strdup (new_location);
    }
  CalOccurrence shown = { edited, edited->start, edited->end };
  g_autofree char *line = line_for (&shown);
  /* a repeating event changes only the showing that was meant */
  calendar_apply_edit (calendar_default (), ev->id, occ, ev->repeat == CAL_REPEAT_NONE ? CAL_SCOPE_ALL : CAL_SCOPE_ONE, edited);
  matches_free (&m);
  *ok = TRUE;
  return g_strdup_printf ("%s %s", TR ("Cambiado:", "Changed:"), line + 2);
}

char *
calendar_tool_delete (const char *event, gboolean *ok)
{
  *ok = FALSE;
  Matches m;
  gint64 occ;
  g_autofree char *problem = NULL;
  const CalEvent *ev = unique_match (event, &m, &occ, &problem);
  if (!ev)
    {
      matches_free (&m);
      return g_steal_pointer (&problem);
    }
  CalOccurrence shown = { ev, occ, occ + (ev->end - ev->start) };
  g_autofree char *line = line_for (&shown);
  gboolean repeats = ev->repeat != CAL_REPEAT_NONE;
  calendar_apply_delete (calendar_default (), ev->id, occ, repeats ? CAL_SCOPE_ONE : CAL_SCOPE_ALL);
  matches_free (&m);
  *ok = TRUE;
  return g_strdup_printf ("%s %s%s", TR ("Borrado:", "Deleted:"), line + 2,
                          repeats ? TR (" (solo esa vez). El usuario puede pedir deshacerlo.", " (only that time). The user can ask to undo it.")
                                  : TR (". El usuario puede pedir deshacerlo.", ". The user can ask to undo it."));
}

char *
calendar_tool_undo (gboolean *ok)
{
  g_autofree char *title = NULL;
  *ok = calendar_undo_delete (calendar_default (), &title);
  if (!*ok)
    return g_strdup (TR ("No hay nada que deshacer.", "There is nothing to undo."));
  return g_strdup_printf ("%s %s", TR ("Restaurado:", "Restored:"), title);
}

char *
calendar_tool_done (const char *event, gboolean *ok)
{
  *ok = FALSE;
  Matches m;
  gint64 occ;
  g_autofree char *problem = NULL;
  const CalEvent *ev = unique_match (event, &m, &occ, &problem);
  if (!ev)
    {
      matches_free (&m);
      return g_steal_pointer (&problem);
    }
  if (!ev->reminder)
    {
      matches_free (&m);
      return g_strdup ("Error: that is an event, not a reminder.");
    }
  g_autofree char *title = g_strdup (ev->title);
  calendar_set_done (calendar_default (), ev->id, occ, TRUE);
  matches_free (&m);
  *ok = TRUE;
  return g_strdup_printf ("%s %s", TR ("Marcado como hecho:", "Marked done:"), title);
}

/* ---- free time ---------------------------------------------------------- */

char *
calendar_tool_free (const char *minutes, const char *when)
{
  int need = minutes ? atoi (minutes) : 0;
  if (need < 5 || need > 12 * 60)
    need = 60;
  gint64 from, to;
  if (!range_of (when && *when ? when : "week", &from, &to))
    return g_strdup ("Error: say today, tomorrow, week, next week, or a date like 2026-10-09.");
  from = MAX (from, now_unix ());
  const CalSettings *cs = calendar_settings ();
  GArray *slots = calendar_free_slots (calendar_default (), from, to, need, cs->day_start, cs->day_end);
  GString *out = g_string_new (NULL);
  guint shown = 0;
  for (guint i = 0; i + 1 < slots->len && shown < 8; i += 2)
    {
      gint64 a = g_array_index (slots, gint64, i), b = g_array_index (slots, gint64, i + 1);
      g_autoptr (GDateTime) da = g_date_time_new_from_unix_local (a);
      g_autoptr (GDateTime) db = g_date_time_new_from_unix_local (b);
      int dow = g_date_time_get_day_of_week (da) - 1, month = g_date_time_get_month (da) - 1;
      g_autofree char *t1 = g_date_time_format (da, "%H:%M");
      g_autofree char *t2 = g_date_time_format (db, "%H:%M");
      g_string_append_printf (out, "- %s %d %s, %s–%s (%d min)\n", english () ? days_en[dow] : days_es[dow],
                              g_date_time_get_day_of_month (da), english () ? months_en[month] : months_es[month], t1, t2,
                              (int) ((b - a) / 60));
      shown++;
    }
  g_array_free (slots, TRUE);
  if (!shown)
    {
      g_string_free (out, TRUE);
      return g_strdup (TR ("No hay un hueco así de largo en ese período.", "There is no gap that long in that period."));
    }
  return g_string_free (out, FALSE);
}
