#include "calendar.h"

#include <json-glib/json-glib.h>
#include <string.h>
#include <stdio.h>

struct _Calendar {
  char      *path;       /* NULL: keep in memory only */
  GPtrArray *events;     /* CalEvent*, in insertion order */
  GPtrArray *calendars;  /* CalCalendar* */
};

static const char *color_hex[CAL_N_COLORS] = { "#3584e4", "#2ec27e", "#ff7800", "#9141ac",
                                               "#e01b24", "#1c9c9c", "#c64600", "#865e3c" };

const char *
calendar_color_hex (guint color)
{
  return color_hex[color % CAL_N_COLORS];
}

static void
calendar_free_one (CalCalendar *c)
{
  g_free (c->id);
  g_free (c->name);
  g_free (c);
}

/* ---- events ------------------------------------------------------------- */

CalEvent *
calendar_event_new (const char *title, gint64 start, gint64 end, gboolean all_day)
{
  CalEvent *ev = g_new0 (CalEvent, 1);
  ev->title = g_strdup (title ? title : "");
  ev->notes = g_strdup ("");
  ev->location = g_strdup ("");
  ev->calendar = NULL;
  ev->alert = CAL_NO_ALERT;
  ev->start = start;
  ev->end = end > start ? end : (all_day ? calendar_day_next (start) : start + 3600);
  ev->all_day = all_day;
  return ev;
}

void
calendar_event_free (CalEvent *ev)
{
  if (!ev)
    return;
  g_free (ev->id);
  g_free (ev->calendar);
  g_free (ev->title);
  g_free (ev->notes);
  g_free (ev->location);
  if (ev->exceptions)
    g_array_free (ev->exceptions, TRUE);
  g_free (ev);
}

CalEvent *
calendar_event_copy (const CalEvent *ev)
{
  CalEvent *c = g_memdup2 (ev, sizeof *ev);
  c->id = g_strdup (ev->id);
  c->calendar = g_strdup (ev->calendar);
  c->title = g_strdup (ev->title);
  c->notes = g_strdup (ev->notes);
  c->location = g_strdup (ev->location);
  c->exceptions = NULL;
  if (ev->exceptions)
    {
      c->exceptions = g_array_new (FALSE, FALSE, sizeof (gint64));
      g_array_append_vals (c->exceptions, ev->exceptions->data, ev->exceptions->len);
    }
  return c;
}

/* ---- days --------------------------------------------------------------- */

gint64
calendar_day_start (gint64 t)
{
  g_autoptr (GDateTime) d = g_date_time_new_from_unix_local (t);
  g_autoptr (GDateTime) m = g_date_time_new_local (g_date_time_get_year (d), g_date_time_get_month (d),
                                                    g_date_time_get_day_of_month (d), 0, 0, 0);
  return g_date_time_to_unix (m);
}

gint64
calendar_day_next (gint64 t)
{
  g_autoptr (GDateTime) d = g_date_time_new_from_unix_local (calendar_day_start (t));
  g_autoptr (GDateTime) n = g_date_time_add_days (d, 1);
  return g_date_time_to_unix (n);
}

static void
add_exception (CalEvent *ev, gint64 occ_start)
{
  if (!ev->exceptions)
    ev->exceptions = g_array_new (FALSE, FALSE, sizeof (gint64));
  g_array_append_val (ev->exceptions, occ_start);
}

static gboolean
is_exception (const CalEvent *ev, gint64 occ_start)
{
  for (guint i = 0; ev->exceptions && i < ev->exceptions->len; i++)
    if (g_array_index (ev->exceptions, gint64, i) == occ_start)
      return TRUE;
  return FALSE;
}

/* ---- file --------------------------------------------------------------- */

static const char *repeat_names[] = { "none", "daily", "weekly", "monthly", "yearly" };

static void
save (Calendar *cal)
{
  if (!cal->path)
    return;
  g_autoptr (JsonBuilder) b = json_builder_new ();
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "calendars");
  json_builder_begin_array (b);
  for (guint i = 0; i < cal->calendars->len; i++)
    {
      const CalCalendar *c = cal->calendars->pdata[i];
      json_builder_begin_object (b);
      json_builder_set_member_name (b, "id");
      json_builder_add_string_value (b, c->id);
      json_builder_set_member_name (b, "name");
      json_builder_add_string_value (b, c->name);
      json_builder_set_member_name (b, "color");
      json_builder_add_int_value (b, c->color);
      json_builder_set_member_name (b, "visible");
      json_builder_add_boolean_value (b, c->visible);
      json_builder_end_object (b);
    }
  json_builder_end_array (b);
  json_builder_set_member_name (b, "events");
  json_builder_begin_array (b);
  for (guint i = 0; i < cal->events->len; i++)
    {
      const CalEvent *e = cal->events->pdata[i];
      json_builder_begin_object (b);
      json_builder_set_member_name (b, "id");
      json_builder_add_string_value (b, e->id);
      json_builder_set_member_name (b, "calendar");
      json_builder_add_string_value (b, e->calendar ? e->calendar : "");
      json_builder_set_member_name (b, "location");
      json_builder_add_string_value (b, e->location);
      json_builder_set_member_name (b, "alert");
      json_builder_add_int_value (b, e->alert);
      json_builder_set_member_name (b, "title");
      json_builder_add_string_value (b, e->title);
      json_builder_set_member_name (b, "notes");
      json_builder_add_string_value (b, e->notes);
      json_builder_set_member_name (b, "start");
      json_builder_add_int_value (b, e->start);
      json_builder_set_member_name (b, "end");
      json_builder_add_int_value (b, e->end);
      json_builder_set_member_name (b, "all_day");
      json_builder_add_boolean_value (b, e->all_day);
      json_builder_set_member_name (b, "repeat");
      json_builder_add_string_value (b, repeat_names[e->repeat]);
      json_builder_set_member_name (b, "until");
      json_builder_add_int_value (b, e->until);
      json_builder_set_member_name (b, "exceptions");
      json_builder_begin_array (b);
      for (guint x = 0; e->exceptions && x < e->exceptions->len; x++)
        json_builder_add_int_value (b, g_array_index (e->exceptions, gint64, x));
      json_builder_end_array (b);
      json_builder_end_object (b);
    }
  json_builder_end_array (b);
  json_builder_end_object (b);

  g_autoptr (JsonGenerator) gen = json_generator_new ();
  g_autoptr (JsonNode) root = json_builder_get_root (b);
  json_generator_set_root (gen, root);
  json_generator_set_pretty (gen, TRUE);
  g_autofree char *text = json_generator_to_data (gen, NULL);

  g_autofree char *dir = g_path_get_dirname (cal->path);
  g_mkdir_with_parents (dir, 0700);
  g_autoptr (GError) error = NULL;
  if (!g_file_set_contents (cal->path, text, -1, &error))
    g_warning ("calendar: could not save %s: %s", cal->path, error->message);
}

static void
load (Calendar *cal)
{
  g_autoptr (JsonParser) parser = json_parser_new ();
  if (!json_parser_load_from_file (parser, cal->path, NULL))
    return;
  JsonNode *root = json_parser_get_root (parser);
  if (!root || !JSON_NODE_HOLDS_OBJECT (root))
    return;
  JsonArray *cals = json_object_get_array_member (json_node_get_object (root), "calendars");
  for (guint i = 0; cals && i < json_array_get_length (cals); i++)
    {
      JsonNode *n = json_array_get_element (cals, i);
      if (!JSON_NODE_HOLDS_OBJECT (n))
        continue;
      JsonObject *o = json_node_get_object (n);
      CalCalendar *c = g_new0 (CalCalendar, 1);
      c->id = g_strdup (json_object_get_string_member_with_default (o, "id", ""));
      if (!*c->id)
        {
          g_free (c->id);
          c->id = g_uuid_string_random ();
        }
      c->name = g_strdup (json_object_get_string_member_with_default (o, "name", "Calendar"));
      c->color = (guint) json_object_get_int_member_with_default (o, "color", 0) % CAL_N_COLORS;
      c->visible = json_object_get_boolean_member_with_default (o, "visible", TRUE);
      g_ptr_array_add (cal->calendars, c);
    }
  JsonArray *list = json_object_get_array_member (json_node_get_object (root), "events");
  if (!list)
    return;
  for (guint i = 0; i < json_array_get_length (list); i++)
    {
      JsonNode *n = json_array_get_element (list, i);
      if (!JSON_NODE_HOLDS_OBJECT (n))
        continue;
      JsonObject *o = json_node_get_object (n);
      gint64 start = json_object_get_int_member_with_default (o, "start", 0);
      gint64 end = json_object_get_int_member_with_default (o, "end", 0);
      if (!start)
        continue;
      gboolean all_day = json_object_get_boolean_member_with_default (o, "all_day", FALSE);
      CalEvent *e = calendar_event_new (json_object_get_string_member_with_default (o, "title", ""), start, end, all_day);
      g_free (e->id);
      e->id = g_strdup (json_object_get_string_member_with_default (o, "id", ""));
      if (!*e->id)
        {
          g_free (e->id);
          e->id = g_uuid_string_random ();
        }
      g_free (e->notes);
      e->notes = g_strdup (json_object_get_string_member_with_default (o, "notes", ""));
      g_free (e->location);
      e->location = g_strdup (json_object_get_string_member_with_default (o, "location", ""));
      e->alert = (int) json_object_get_int_member_with_default (o, "alert", CAL_NO_ALERT);
      const char *cid = json_object_get_string_member_with_default (o, "calendar", "");
      e->calendar = *cid ? g_strdup (cid) : NULL;
      const char *rep = json_object_get_string_member_with_default (o, "repeat", "none");
      for (guint r = 0; r < G_N_ELEMENTS (repeat_names); r++)
        if (g_str_equal (rep, repeat_names[r]))
          e->repeat = (CalRepeat) r;
      e->until = json_object_get_int_member_with_default (o, "until", 0);
      JsonArray *ex = json_object_get_array_member (o, "exceptions");
      for (guint x = 0; ex && x < json_array_get_length (ex); x++)
        add_exception (e, json_array_get_int_element (ex, x));
      g_ptr_array_add (cal->events, e);
    }
}

static CalCalendar *
calendar_calendar_find_exact (Calendar *cal, const char *id)
{
  for (guint i = 0; id && i < cal->calendars->len; i++)
    if (g_str_equal (((CalCalendar *) cal->calendars->pdata[i])->id, id))
      return cal->calendars->pdata[i];
  return NULL;
}

Calendar *
calendar_new (const char *path)
{
  Calendar *cal = g_new0 (Calendar, 1);
  cal->path = g_strdup (path);
  cal->events = g_ptr_array_new_with_free_func ((GDestroyNotify) calendar_event_free);
  cal->calendars = g_ptr_array_new_with_free_func ((GDestroyNotify) calendar_free_one);
  if (path)
    load (cal);
  if (cal->calendars->len == 0)
    {
      CalCalendar *c = g_new0 (CalCalendar, 1);
      c->id = g_strdup ("personal");
      c->name = g_strdup ("Personal");
      c->color = 0;
      c->visible = TRUE;
      g_ptr_array_add (cal->calendars, c);
    }
  /* every event belongs to a calendar that exists */
  for (guint i = 0; i < cal->events->len; i++)
    {
      CalEvent *e = cal->events->pdata[i];
      if (!e->calendar || !calendar_calendar_find_exact (cal, e->calendar))
        {
          g_free (e->calendar);
          e->calendar = g_strdup (((CalCalendar *) cal->calendars->pdata[0])->id);
        }
    }
  return cal;
}

void
calendar_free (Calendar *cal)
{
  if (!cal)
    return;
  g_ptr_array_unref (cal->events);
  g_ptr_array_unref (cal->calendars);
  g_free (cal->path);
  g_free (cal);
}

/* ---- calendars ---------------------------------------------------------- */

GPtrArray *
calendar_calendars (Calendar *cal)
{
  return cal->calendars;
}

CalCalendar *
calendar_calendar_find (Calendar *cal, const char *id)
{
  CalCalendar *c = calendar_calendar_find_exact (cal, id);
  return c ? c : cal->calendars->pdata[0];
}

const char *
calendar_calendar_add (Calendar *cal, const char *name, guint color)
{
  CalCalendar *c = g_new0 (CalCalendar, 1);
  c->id = g_uuid_string_random ();
  c->name = g_strdup (name);
  c->color = color % CAL_N_COLORS;
  c->visible = TRUE;
  g_ptr_array_add (cal->calendars, c);
  save (cal);
  return c->id;
}

void
calendar_calendar_changed (Calendar *cal)
{
  save (cal);
}

gboolean
calendar_calendar_remove (Calendar *cal, const char *id)
{
  CalCalendar *c = calendar_calendar_find_exact (cal, id);
  if (!c || cal->calendars->len < 2)
    return FALSE;
  for (guint i = cal->events->len; i > 0; i--)
    if (g_str_equal (((CalEvent *) cal->events->pdata[i - 1])->calendar, id))
      g_ptr_array_remove_index (cal->events, i - 1);
  g_ptr_array_remove (cal->calendars, c);
  save (cal);
  return TRUE;
}

/* ---- editing ------------------------------------------------------------ */

const char *
calendar_add (Calendar *cal, CalEvent *ev)
{
  g_free (ev->id);
  ev->id = g_uuid_string_random ();
  if (!ev->calendar || !calendar_calendar_find_exact (cal, ev->calendar))
    {
      g_free (ev->calendar);
      ev->calendar = g_strdup (((CalCalendar *) cal->calendars->pdata[0])->id);
    }
  g_ptr_array_add (cal->events, ev);
  save (cal);
  return ev->id;
}

gboolean
calendar_update (Calendar *cal, CalEvent *ev)
{
  for (guint i = 0; i < cal->events->len; i++)
    if (g_str_equal (((CalEvent *) cal->events->pdata[i])->id, ev->id))
      {
        calendar_event_free (cal->events->pdata[i]);
        cal->events->pdata[i] = ev;
        save (cal);
        return TRUE;
      }
  calendar_event_free (ev);
  return FALSE;
}

gboolean
calendar_remove (Calendar *cal, const char *id)
{
  for (guint i = 0; id && i < cal->events->len; i++)
    if (g_str_equal (((CalEvent *) cal->events->pdata[i])->id, id))
      {
        g_ptr_array_remove_index (cal->events, i);
        save (cal);
        return TRUE;
      }
  return FALSE;
}

const CalEvent *
calendar_find (Calendar *cal, const char *id)
{
  for (guint i = 0; id && i < cal->events->len; i++)
    if (g_str_equal (((CalEvent *) cal->events->pdata[i])->id, id))
      return cal->events->pdata[i];
  return NULL;
}

guint
calendar_count (Calendar *cal)
{
  return cal->events->len;
}

/* ---- occurrences -------------------------------------------------------- */

/* The n-th repeat of ev, counted from its first showing. Months and years are
 * always computed from the original date, so the 31st does not drift. */
static gint64
nth_start (const CalEvent *ev, guint n)
{
  g_autoptr (GDateTime) first = g_date_time_new_from_unix_local (ev->start);
  g_autoptr (GDateTime) d = NULL;
  switch (ev->repeat)
    {
    case CAL_REPEAT_DAILY:   d = g_date_time_add_days (first, (gint) n); break;
    case CAL_REPEAT_WEEKLY:  d = g_date_time_add_weeks (first, (gint) n); break;
    case CAL_REPEAT_MONTHLY: d = g_date_time_add_months (first, (gint) n); break;
    case CAL_REPEAT_YEARLY:  d = g_date_time_add_years (first, (gint) n); break;
    default:                 return ev->start;
    }
  return g_date_time_to_unix (d);
}

static int
by_start (gconstpointer a, gconstpointer b)
{
  const CalOccurrence *x = a, *y = b;
  if (x->start != y->start)
    return x->start < y->start ? -1 : 1;
  return x->end < y->end ? -1 : (x->end > y->end);
}

GArray *
calendar_occurrences (Calendar *cal, gint64 from, gint64 to)
{
  GArray *out = g_array_new (FALSE, FALSE, sizeof (CalOccurrence));
  for (guint i = 0; i < cal->events->len; i++)
    {
      const CalEvent *ev = cal->events->pdata[i];
      gint64 length = ev->end - ev->start;
      guint n = 0;
      if (ev->repeat == CAL_REPEAT_DAILY || ev->repeat == CAL_REPEAT_WEEKLY)
        {
          /* skip straight to the neighbourhood of the range; the margin covers daylight saving */
          gint64 step = ev->repeat == CAL_REPEAT_DAILY ? 86400 : 7 * 86400;
          gint64 skip = (from - length - ev->start) / step - 2;
          n = skip > 0 ? (guint) skip : 0;
        }
      for (;; n++)
        {
          gint64 s = ev->repeat == CAL_REPEAT_NONE ? ev->start : nth_start (ev, n);
          if (s >= to || (ev->until && ev->repeat != CAL_REPEAT_NONE && s > ev->until + 86399))
            break;
          if (s + length > from && !is_exception (ev, s))
            {
              CalOccurrence o = { ev, s, s + length };
              g_array_append_val (out, o);
            }
          if (ev->repeat == CAL_REPEAT_NONE || n > 100000)
            break;
        }
    }
  g_array_sort (out, by_start);
  return out;
}

gboolean
calendar_next_alert (Calendar *cal, gint64 after, CalAlert *out)
{
  /* an alert is at most a day before its event, so showings starting within ten days are enough */
  GArray *occ = calendar_occurrences (cal, after, after + 10 * 86400);
  gboolean found = FALSE;
  for (guint i = 0; i < occ->len; i++)
    {
      const CalOccurrence *o = &g_array_index (occ, CalOccurrence, i);
      if (o->event->alert == CAL_NO_ALERT)
        continue;
      gint64 fire = o->start - (gint64) o->event->alert * 60;
      if (fire <= after || (found && fire >= out->fire))
        continue;
      out->event = o->event;
      out->fire = fire;
      out->start = o->start;
      found = TRUE;
    }
  g_array_free (occ, TRUE);
  return found;
}

/* ---- editing one showing of a repeating event --------------------------- */

static CalEvent *
find_mutable (Calendar *cal, const char *id)
{
  for (guint i = 0; id && i < cal->events->len; i++)
    if (g_str_equal (((CalEvent *) cal->events->pdata[i])->id, id))
      return cal->events->pdata[i];
  return NULL;
}

/* the repeating event stops before this showing; FALSE when this was its first showing */
static gboolean
truncate_before (CalEvent *master, gint64 occ_start)
{
  if (occ_start <= master->start)
    return FALSE;
  master->until = calendar_day_start (calendar_day_start (occ_start) - 1);
  return TRUE;
}

gboolean
calendar_apply_delete (Calendar *cal, const char *id, gint64 occ_start, CalScope scope)
{
  CalEvent *master = find_mutable (cal, id);
  if (!master)
    return FALSE;
  if (master->repeat == CAL_REPEAT_NONE || scope == CAL_SCOPE_ALL)
    return calendar_remove (cal, id);
  if (scope == CAL_SCOPE_ONE)
    add_exception (master, occ_start);
  else if (!truncate_before (master, occ_start))
    return calendar_remove (cal, id);
  save (cal);
  return TRUE;
}

gboolean
calendar_apply_edit (Calendar *cal, const char *id, gint64 occ_start, CalScope scope, CalEvent *edited)
{
  CalEvent *master = find_mutable (cal, id);
  if (!master)
    {
      calendar_event_free (edited);
      return FALSE;
    }
  if (master->repeat == CAL_REPEAT_NONE || scope == CAL_SCOPE_ALL)
    {
      /* the whole event takes the new values; its first showing moves by as much as this one did */
      gint64 shift = edited->start - occ_start;
      gint64 length = edited->end - edited->start;
      edited->start = master->start + shift;
      edited->end = edited->start + length;
      g_free (edited->id);
      edited->id = g_strdup (id);
      if (edited->exceptions == NULL && master->exceptions)
        {
          edited->exceptions = g_array_new (FALSE, FALSE, sizeof (gint64));
          g_array_append_vals (edited->exceptions, master->exceptions->data, master->exceptions->len);
        }
      return calendar_update (cal, edited);
    }
  if (scope == CAL_SCOPE_ONE)
    {
      add_exception (master, occ_start);
      edited->repeat = CAL_REPEAT_NONE;
      edited->until = 0;
      calendar_add (cal, edited);   /* saves */
      return TRUE;
    }
  /* this showing and the ones after it: the old series stops, a new one starts here */
  if (!truncate_before (master, occ_start))
    {
      g_autofree char *keep = g_strdup (id);
      calendar_remove (cal, keep);
    }
  calendar_add (cal, edited);
  return TRUE;
}

/* ---- parsing for the model --------------------------------------------- */

gboolean
calendar_parse_time (const char *text, gint64 *t, gboolean *date_only)
{
  int y, mo, d, h = 0, mi = 0;
  if (!text)
    return FALSE;
  g_autofree char *s = g_strstrip (g_strdup (text));
  int n = sscanf (s, "%4d-%2d-%2d%*1[T ]%2d:%2d", &y, &mo, &d, &h, &mi);
  if (n != 3 && n != 5)
    return FALSE;
  g_autoptr (GDateTime) dt = g_date_time_new_local (y, mo, d, n == 5 ? h : 0, n == 5 ? mi : 0, 0);
  if (!dt)
    return FALSE;
  *t = g_date_time_to_unix (dt);
  if (date_only)
    *date_only = n == 3;
  return TRUE;
}

/* ---- the app's calendar ------------------------------------------------- */

static Calendar *default_calendar;

Calendar *
calendar_default (void)
{
  if (!default_calendar)
    {
      g_autofree char *path = g_build_filename (g_get_user_data_dir (), "npu-chat", "calendar.json", NULL);
      default_calendar = calendar_new (path);
    }
  return default_calendar;
}

void
calendar_default_free (void)
{
  calendar_free (default_calendar);
  default_calendar = NULL;
}
