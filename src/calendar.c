#include "calendar.h"

#include <json-glib/json-glib.h>
#include <string.h>
#include <stdio.h>

struct _Calendar {
  char   *path;       /* NULL: keep in memory only */
  GPtrArray *events;  /* CalEvent*, in insertion order */
};

/* ---- events ------------------------------------------------------------- */

CalEvent *
calendar_event_new (const char *title, gint64 start, gint64 end, gboolean all_day)
{
  CalEvent *ev = g_new0 (CalEvent, 1);
  ev->title = g_strdup (title ? title : "");
  ev->notes = g_strdup ("");
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
  g_free (ev->title);
  g_free (ev->notes);
  g_free (ev);
}

CalEvent *
calendar_event_copy (const CalEvent *ev)
{
  CalEvent *c = g_memdup2 (ev, sizeof *ev);
  c->id = g_strdup (ev->id);
  c->title = g_strdup (ev->title);
  c->notes = g_strdup (ev->notes);
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

/* ---- file --------------------------------------------------------------- */

static const char *repeat_names[] = { "none", "daily", "weekly", "monthly", "yearly" };

static void
save (Calendar *cal)
{
  if (!cal->path)
    return;
  g_autoptr (JsonBuilder) b = json_builder_new ();
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "events");
  json_builder_begin_array (b);
  for (guint i = 0; i < cal->events->len; i++)
    {
      const CalEvent *e = cal->events->pdata[i];
      json_builder_begin_object (b);
      json_builder_set_member_name (b, "id");
      json_builder_add_string_value (b, e->id);
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
      const char *rep = json_object_get_string_member_with_default (o, "repeat", "none");
      for (guint r = 0; r < G_N_ELEMENTS (repeat_names); r++)
        if (g_str_equal (rep, repeat_names[r]))
          e->repeat = (CalRepeat) r;
      e->until = json_object_get_int_member_with_default (o, "until", 0);
      g_ptr_array_add (cal->events, e);
    }
}

Calendar *
calendar_new (const char *path)
{
  Calendar *cal = g_new0 (Calendar, 1);
  cal->path = g_strdup (path);
  cal->events = g_ptr_array_new_with_free_func ((GDestroyNotify) calendar_event_free);
  if (path)
    load (cal);
  return cal;
}

void
calendar_free (Calendar *cal)
{
  if (!cal)
    return;
  g_ptr_array_unref (cal->events);
  g_free (cal->path);
  g_free (cal);
}

/* ---- editing ------------------------------------------------------------ */

const char *
calendar_add (Calendar *cal, CalEvent *ev)
{
  g_free (ev->id);
  ev->id = g_uuid_string_random ();
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
          if (s + length > from)
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
