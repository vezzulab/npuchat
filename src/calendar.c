#include "calendar.h"

#include <json-glib/json-glib.h>
#include <stdio.h>
#include <string.h>

#define TRASH_LIMIT 30

struct _Calendar {
  char      *path;       /* NULL: keep in memory only */
  GPtrArray *events;     /* CalEvent*, in insertion order */
  GPtrArray *calendars;  /* CalCalendar* */
  GPtrArray *trash;      /* CalEvent*, the most recent deletion last */
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
  g_free (c->url);
  g_free (c);
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

/* ---- events ------------------------------------------------------------- */

CalEvent *
calendar_event_new (const char *title, gint64 start, gint64 end, gboolean all_day)
{
  CalEvent *ev = g_new0 (CalEvent, 1);
  ev->title = g_strdup (title ? title : "");
  ev->notes = g_strdup ("");
  ev->location = g_strdup ("");
  ev->url = g_strdup ("");
  ev->alerts = g_array_new (FALSE, FALSE, sizeof (int));
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
  g_free (ev->uid);
  g_free (ev->calendar);
  g_free (ev->title);
  g_free (ev->notes);
  g_free (ev->location);
  g_free (ev->url);
  if (ev->alerts)
    g_array_free (ev->alerts, TRUE);
  if (ev->exceptions)
    g_array_free (ev->exceptions, TRUE);
  if (ev->completed)
    g_array_free (ev->completed, TRUE);
  g_free (ev);
}

static GArray *
copy_array (const GArray *a, guint size)
{
  if (!a)
    return NULL;
  GArray *c = g_array_new (FALSE, FALSE, size);
  g_array_append_vals (c, a->data, a->len);
  return c;
}

CalEvent *
calendar_event_copy (const CalEvent *ev)
{
  CalEvent *c = g_memdup2 (ev, sizeof *ev);
  c->id = g_strdup (ev->id);
  c->uid = g_strdup (ev->uid);
  c->calendar = g_strdup (ev->calendar);
  c->title = g_strdup (ev->title);
  c->notes = g_strdup (ev->notes);
  c->location = g_strdup (ev->location);
  c->url = g_strdup (ev->url);
  c->alerts = copy_array (ev->alerts, sizeof (int));
  c->exceptions = copy_array (ev->exceptions, sizeof (gint64));
  c->completed = copy_array (ev->completed, sizeof (gint64));
  return c;
}

void
calendar_event_set_alerts (CalEvent *ev, const int *minutes, guint n)
{
  g_array_set_size (ev->alerts, 0);
  for (guint i = 0; i < n; i++)
    if (minutes[i] >= 0)
      g_array_append_val (ev->alerts, minutes[i]);
}

void
calendar_event_add_alert (CalEvent *ev, int minutes)
{
  if (minutes >= 0)
    g_array_append_val (ev->alerts, minutes);
}

int
calendar_event_alert (const CalEvent *ev, guint index)
{
  return index < ev->alerts->len ? g_array_index (ev->alerts, int, index) : -1;
}

guint
calendar_event_alert_count (const CalEvent *ev)
{
  return ev->alerts->len;
}

static void
add_time (GArray **list, gint64 t)
{
  if (!*list)
    *list = g_array_new (FALSE, FALSE, sizeof (gint64));
  g_array_append_val (*list, t);
}

static gboolean
has_time (const GArray *list, gint64 t)
{
  for (guint i = 0; list && i < list->len; i++)
    if (g_array_index (list, gint64, i) == t)
      return TRUE;
  return FALSE;
}

gboolean
calendar_is_done (const CalEvent *ev, gint64 occ_start)
{
  return ev->repeat == CAL_REPEAT_NONE ? ev->done : has_time (ev->completed, occ_start);
}

/* ---- file --------------------------------------------------------------- */

static const char *repeat_names[] = { "none", "daily", "weekly", "monthly", "yearly" };
static const char *monthly_names[] = { "date", "weekday", "lastday" };

static void
write_times (JsonBuilder *b, const char *name, const GArray *list)
{
  json_builder_set_member_name (b, name);
  json_builder_begin_array (b);
  for (guint i = 0; list && i < list->len; i++)
    json_builder_add_int_value (b, g_array_index (list, gint64, i));
  json_builder_end_array (b);
}

static void
write_event (JsonBuilder *b, const CalEvent *e)
{
  json_builder_begin_object (b);
#define STR(k, v) json_builder_set_member_name (b, k); json_builder_add_string_value (b, (v) ? (v) : "")
#define INT(k, v) json_builder_set_member_name (b, k); json_builder_add_int_value (b, v)
#define BOOL(k, v) json_builder_set_member_name (b, k); json_builder_add_boolean_value (b, v)
  STR ("id", e->id);
  STR ("uid", e->uid);
  STR ("calendar", e->calendar);
  STR ("title", e->title);
  STR ("notes", e->notes);
  STR ("location", e->location);
  STR ("url", e->url);
  INT ("start", e->start);
  INT ("end", e->end);
  BOOL ("all_day", e->all_day);
  BOOL ("reminder", e->reminder);
  BOOL ("done", e->done);
  STR ("repeat", repeat_names[e->repeat]);
  INT ("interval", e->interval);
  INT ("weekdays", e->weekdays);
  STR ("monthly", monthly_names[e->monthly]);
  INT ("count", e->count);
  INT ("until", e->until);
  json_builder_set_member_name (b, "alerts");
  json_builder_begin_array (b);
  for (guint i = 0; i < e->alerts->len; i++)
    json_builder_add_int_value (b, g_array_index (e->alerts, int, i));
  json_builder_end_array (b);
  write_times (b, "exceptions", e->exceptions);
  write_times (b, "completed", e->completed);
  json_builder_end_object (b);
#undef STR
#undef INT
#undef BOOL
}

static CalEvent *
read_event (JsonObject *o)
{
  gint64 start = json_object_get_int_member_with_default (o, "start", 0);
  if (!start)
    return NULL;
  gint64 end = json_object_get_int_member_with_default (o, "end", 0);
  gboolean all_day = json_object_get_boolean_member_with_default (o, "all_day", FALSE);
  CalEvent *e = calendar_event_new (json_object_get_string_member_with_default (o, "title", ""), start, end, all_day);
  g_free (e->id);
  e->id = g_strdup (json_object_get_string_member_with_default (o, "id", ""));
  if (!*e->id)
    {
      g_free (e->id);
      e->id = g_uuid_string_random ();
    }
#define TEXT(field, key) g_free (e->field); e->field = g_strdup (json_object_get_string_member_with_default (o, key, ""))
  TEXT (notes, "notes");
  TEXT (location, "location");
  TEXT (url, "url");
#undef TEXT
  const char *uid = json_object_get_string_member_with_default (o, "uid", "");
  e->uid = *uid ? g_strdup (uid) : NULL;
  const char *cid = json_object_get_string_member_with_default (o, "calendar", "");
  e->calendar = *cid ? g_strdup (cid) : NULL;
  e->reminder = json_object_get_boolean_member_with_default (o, "reminder", FALSE);
  e->done = json_object_get_boolean_member_with_default (o, "done", FALSE);
  const char *rep = json_object_get_string_member_with_default (o, "repeat", "none");
  for (guint r = 0; r < G_N_ELEMENTS (repeat_names); r++)
    if (g_str_equal (rep, repeat_names[r]))
      e->repeat = (CalRepeat) r;
  const char *mon = json_object_get_string_member_with_default (o, "monthly", "date");
  for (guint r = 0; r < G_N_ELEMENTS (monthly_names); r++)
    if (g_str_equal (mon, monthly_names[r]))
      e->monthly = (CalMonthly) r;
  e->interval = (guint) json_object_get_int_member_with_default (o, "interval", 0);
  e->weekdays = (guint) json_object_get_int_member_with_default (o, "weekdays", 0) & 0x7f;
  e->count = (int) json_object_get_int_member_with_default (o, "count", 0);
  e->until = json_object_get_int_member_with_default (o, "until", 0);
  JsonArray *al = json_object_get_array_member (o, "alerts");
  for (guint x = 0; al && x < json_array_get_length (al); x++)
    calendar_event_add_alert (e, (int) json_array_get_int_element (al, x));
  if (!al && json_object_has_member (o, "alert"))   /* files from before there were several */
    calendar_event_add_alert (e, (int) json_object_get_int_member (o, "alert"));
  JsonArray *ex = json_object_get_array_member (o, "exceptions");
  for (guint x = 0; ex && x < json_array_get_length (ex); x++)
    add_time (&e->exceptions, json_array_get_int_element (ex, x));
  JsonArray *co = json_object_get_array_member (o, "completed");
  for (guint x = 0; co && x < json_array_get_length (co); x++)
    add_time (&e->completed, json_array_get_int_element (co, x));
  return e;
}

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
      json_builder_set_member_name (b, "url");
      json_builder_add_string_value (b, c->url ? c->url : "");
      json_builder_set_member_name (b, "refresh_hours");
      json_builder_add_int_value (b, c->refresh_hours);
      json_builder_set_member_name (b, "fetched");
      json_builder_add_int_value (b, c->fetched);
      json_builder_end_object (b);
    }
  json_builder_end_array (b);
  json_builder_set_member_name (b, "events");
  json_builder_begin_array (b);
  for (guint i = 0; i < cal->events->len; i++)
    write_event (b, cal->events->pdata[i]);
  json_builder_end_array (b);
  json_builder_set_member_name (b, "trash");
  json_builder_begin_array (b);
  for (guint i = 0; i < cal->trash->len; i++)
    write_event (b, cal->trash->pdata[i]);
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
  JsonObject *top = json_node_get_object (root);
  JsonArray *cals = json_object_get_array_member (top, "calendars");
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
      const char *url = json_object_get_string_member_with_default (o, "url", "");
      c->url = *url ? g_strdup (url) : NULL;
      c->refresh_hours = (int) json_object_get_int_member_with_default (o, "refresh_hours", 0);
      c->fetched = json_object_get_int_member_with_default (o, "fetched", 0);
      g_ptr_array_add (cal->calendars, c);
    }
  JsonArray *list = json_object_get_array_member (top, "events");
  for (guint i = 0; list && i < json_array_get_length (list); i++)
    {
      JsonNode *n = json_array_get_element (list, i);
      CalEvent *e = JSON_NODE_HOLDS_OBJECT (n) ? read_event (json_node_get_object (n)) : NULL;
      if (e)
        g_ptr_array_add (cal->events, e);
    }
  JsonArray *trash = json_object_get_array_member (top, "trash");
  for (guint i = 0; trash && i < json_array_get_length (trash); i++)
    {
      JsonNode *n = json_array_get_element (trash, i);
      CalEvent *e = JSON_NODE_HOLDS_OBJECT (n) ? read_event (json_node_get_object (n)) : NULL;
      if (e)
        g_ptr_array_add (cal->trash, e);
    }
}

static CalCalendar *
find_calendar_exact (Calendar *cal, const char *id)
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
  cal->trash = g_ptr_array_new_with_free_func ((GDestroyNotify) calendar_event_free);
  if (path)
    load (cal);
  if (cal->calendars->len == 0 || !calendar_default_target (cal))
    {
      CalCalendar *c = g_new0 (CalCalendar, 1);
      c->id = g_strdup (cal->calendars->len ? "personal-own" : "personal");
      c->name = g_strdup ("Personal");
      c->visible = TRUE;
      g_ptr_array_insert (cal->calendars, 0, c);
    }
  /* every event belongs to a calendar that exists */
  const CalCalendar *target = calendar_default_target (cal);
  for (guint i = 0; i < cal->events->len; i++)
    {
      CalEvent *e = cal->events->pdata[i];
      if (!e->calendar || !find_calendar_exact (cal, e->calendar))
        {
          g_free (e->calendar);
          e->calendar = g_strdup (target->id);
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
  g_ptr_array_unref (cal->trash);
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
  CalCalendar *c = find_calendar_exact (cal, id);
  return c ? c : cal->calendars->pdata[0];
}

const CalCalendar *
calendar_default_target (Calendar *cal)
{
  for (guint i = 0; i < cal->calendars->len; i++)
    if (!((CalCalendar *) cal->calendars->pdata[i])->url)
      return cal->calendars->pdata[i];
  return NULL;
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

const char *
calendar_subscription_add (Calendar *cal, const char *name, guint color, const char *url, int refresh_hours)
{
  const char *id = calendar_calendar_add (cal, name, color);
  CalCalendar *c = find_calendar_exact (cal, id);
  c->url = g_strdup (url);
  c->refresh_hours = refresh_hours;
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
  CalCalendar *c = find_calendar_exact (cal, id);
  if (!c)
    return FALSE;
  /* the user's own calendars: at least one must stay */
  if (!c->url)
    {
      guint own = 0;
      for (guint i = 0; i < cal->calendars->len; i++)
        own += ((CalCalendar *) cal->calendars->pdata[i])->url == NULL;
      if (own < 2)
        return FALSE;
    }
  for (guint i = cal->events->len; i > 0; i--)
    if (g_str_equal (((CalEvent *) cal->events->pdata[i - 1])->calendar, id))
      g_ptr_array_remove_index (cal->events, i - 1);
  g_ptr_array_remove (cal->calendars, c);
  save (cal);
  return TRUE;
}

void
calendar_replace_events (Calendar *cal, const char *calendar_id, GPtrArray *events)
{
  for (guint i = cal->events->len; i > 0; i--)
    if (g_str_equal (((CalEvent *) cal->events->pdata[i - 1])->calendar, calendar_id))
      g_ptr_array_remove_index (cal->events, i - 1);
  for (guint i = 0; i < events->len; i++)
    {
      CalEvent *e = events->pdata[i];
      g_free (e->calendar);
      e->calendar = g_strdup (calendar_id);
      if (!e->id || !*e->id)
        {
          g_free (e->id);
          e->id = g_uuid_string_random ();
        }
      g_ptr_array_add (cal->events, e);
    }
  g_ptr_array_set_free_func (events, NULL);
  g_ptr_array_set_size (events, 0);
  CalCalendar *c = find_calendar_exact (cal, calendar_id);
  if (c)
    c->fetched = g_get_real_time () / G_USEC_PER_SEC;
  save (cal);
}

/* ---- editing ------------------------------------------------------------ */

const char *
calendar_add (Calendar *cal, CalEvent *ev)
{
  g_free (ev->id);
  ev->id = g_uuid_string_random ();
  /* subscriptions are read-only: a new event goes to a calendar of the user's own */
  CalCalendar *c = find_calendar_exact (cal, ev->calendar);
  if (!c || c->url)
    {
      g_free (ev->calendar);
      ev->calendar = g_strdup (calendar_default_target (cal)->id);
    }
  g_ptr_array_add (cal->events, ev);
  save (cal);
  return ev->id;
}

guint
calendar_import (Calendar *cal, GPtrArray *events, const char *calendar_id)
{
  CalCalendar *c = find_calendar_exact (cal, calendar_id);
  if (!c || c->url)
    c = (CalCalendar *) calendar_default_target (cal);
  guint n = events->len;
  for (guint i = 0; i < n; i++)
    {
      CalEvent *e = events->pdata[i];
      g_free (e->id);
      e->id = g_uuid_string_random ();
      g_free (e->calendar);
      e->calendar = g_strdup (c->id);
      g_ptr_array_add (cal->events, e);
    }
  g_ptr_array_set_free_func (events, NULL);
  g_ptr_array_set_size (events, 0);
  if (n)
    save (cal);
  return n;
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

static void
to_trash (Calendar *cal, CalEvent *ev)
{
  g_ptr_array_add (cal->trash, ev);
  while (cal->trash->len > TRASH_LIMIT)
    g_ptr_array_remove_index (cal->trash, 0);
}

gboolean
calendar_remove (Calendar *cal, const char *id)
{
  for (guint i = 0; id && i < cal->events->len; i++)
    if (g_str_equal (((CalEvent *) cal->events->pdata[i])->id, id))
      {
        CalEvent *ev = g_ptr_array_steal_index (cal->events, i);
        to_trash (cal, ev);
        save (cal);
        return TRUE;
      }
  return FALSE;
}

gboolean
calendar_undo_delete (Calendar *cal, char **title)
{
  if (!cal->trash->len)
    return FALSE;
  CalEvent *ev = g_ptr_array_steal_index (cal->trash, cal->trash->len - 1);
  if (title)
    *title = g_strdup (ev->title);
  g_ptr_array_add (cal->events, ev);
  save (cal);
  return TRUE;
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

const GPtrArray *
calendar_all_events (Calendar *cal)
{
  return cal->events;
}

void
calendar_set_done (Calendar *cal, const char *id, gint64 occ_start, gboolean done)
{
  CalEvent *ev = NULL;
  for (guint i = 0; id && i < cal->events->len; i++)
    if (g_str_equal (((CalEvent *) cal->events->pdata[i])->id, id))
      ev = cal->events->pdata[i];
  if (!ev)
    return;
  if (ev->repeat == CAL_REPEAT_NONE)
    ev->done = done;
  else if (done && !has_time (ev->completed, occ_start))
    add_time (&ev->completed, occ_start);
  else if (!done && ev->completed)
    for (guint i = 0; i < ev->completed->len; i++)
      if (g_array_index (ev->completed, gint64, i) == occ_start)
        {
          g_array_remove_index (ev->completed, i);
          break;
        }
  save (cal);
}

/* ---- occurrences -------------------------------------------------------- */

static int
popcount7 (guint mask)
{
  int n = 0;
  for (int i = 0; i < 7; i++)
    n += (mask >> i) & 1;
  return n;
}

/* The date and time of day of a showing: the day it falls on, at the event's clock time. */
static gint64
on_day (GDateTime *day, GDateTime *clock)
{
  g_autoptr (GDateTime) t = g_date_time_new_local (g_date_time_get_year (day), g_date_time_get_month (day),
                                                    g_date_time_get_day_of_month (day), g_date_time_get_hour (clock),
                                                    g_date_time_get_minute (clock), g_date_time_get_second (clock));
  return g_date_time_to_unix (t);
}

static int
days_in (int year, int month)
{
  return g_date_get_days_in_month ((GDateMonth) month, (GDateYear) year);
}

/* The n-th slot of a repeating event. Slots before the event's own start can occur in
 * the first week of a weekly event with several weekdays; callers skip them. */
static gint64
series_start (const CalEvent *ev, guint n)
{
  guint step = ev->interval ? ev->interval : 1;
  g_autoptr (GDateTime) first = g_date_time_new_from_unix_local (ev->start);
  g_autoptr (GDateTime) d = NULL;
  switch (ev->repeat)
    {
    case CAL_REPEAT_DAILY:
      d = g_date_time_add_days (first, (gint) (n * step));
      return g_date_time_to_unix (d);
    case CAL_REPEAT_WEEKLY:
      if (!ev->weekdays)
        {
          d = g_date_time_add_weeks (first, (gint) (n * step));
          return g_date_time_to_unix (d);
        }
      {
        int k = popcount7 (ev->weekdays), w = (int) n / k, j = (int) n % k, dow = 0;
        for (int b = 0, seen = -1; b < 7; b++)
          if ((ev->weekdays >> b) & 1 && ++seen == j)
            {
              dow = b;
              break;
            }
        int back = g_date_time_get_day_of_week (first) - 1;
        g_autoptr (GDateTime) monday = g_date_time_add_days (first, -back);
        g_autoptr (GDateTime) week = g_date_time_add_weeks (monday, (gint) (w * step));
        g_autoptr (GDateTime) day = g_date_time_add_days (week, dow);
        return on_day (day, first);
      }
    case CAL_REPEAT_MONTHLY:
      {
        g_autoptr (GDateTime) first_of_month = g_date_time_new_local (g_date_time_get_year (first), g_date_time_get_month (first), 1, 0, 0, 0);
        g_autoptr (GDateTime) month = g_date_time_add_months (first_of_month, (gint) (n * step));
        int y = g_date_time_get_year (month), m = g_date_time_get_month (month), dim = days_in (y, m);
        int day = g_date_time_get_day_of_month (first);
        if (ev->monthly == CAL_MONTHLY_LAST_DAY)
          day = dim;
        else if (ev->monthly == CAL_MONTHLY_WEEKDAY)
          {
            int nth = (g_date_time_get_day_of_month (first) - 1) / 7 + 1;
            int want = g_date_time_get_day_of_week (first);
            g_autoptr (GDateTime) one = g_date_time_new_local (y, m, 1, 0, 0, 0);
            int delta = (want - g_date_time_get_day_of_week (one) + 7) % 7;
            day = 1 + delta + 7 * (nth - 1);
            while (day > dim)
              day -= 7;    /* the fifth one means the last one */
          }
        else if (day > dim)
          day = dim;
        g_autoptr (GDateTime) target = g_date_time_new_local (y, m, day, 0, 0, 0);
        return on_day (target, first);
      }
    case CAL_REPEAT_YEARLY:
      d = g_date_time_add_years (first, (gint) (n * step));
      return g_date_time_to_unix (d);
    default:
      return ev->start;
    }
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
      if (ev->repeat == CAL_REPEAT_NONE)
        {
          if (ev->start < to && ev->end > from)
            {
              CalOccurrence o = { ev, ev->start, ev->end };
              g_array_append_val (out, o);
            }
          continue;
        }
      guint n = 0;
      if (!ev->count && (ev->repeat == CAL_REPEAT_DAILY || ev->repeat == CAL_REPEAT_WEEKLY))
        {
          /* skip straight to the neighbourhood of the range; the margin covers daylight saving */
          guint step = ev->interval ? ev->interval : 1;
          gint64 unit = (ev->repeat == CAL_REPEAT_DAILY ? 86400 : 7 * 86400) * (gint64) step;
          gint64 skip = (from - length - ev->start) / unit - 2;
          if (skip > 0)
            n = (guint) skip * (ev->repeat == CAL_REPEAT_WEEKLY && ev->weekdays ? (guint) popcount7 (ev->weekdays) : 1);
        }
      int shown = 0;
      for (guint guard = 0; guard < 200000; n++, guard++)
        {
          gint64 s = series_start (ev, n);
          if (s < ev->start)
            continue;                     /* a slot before the event began */
          if (s >= to || (ev->until && s > ev->until + 86399))
            break;
          if (ev->count && ++shown > ev->count)
            break;
          if (s + length > from && !has_time (ev->exceptions, s))
            {
              CalOccurrence o = { ev, s, s + length };
              g_array_append_val (out, o);
            }
        }
    }
  g_array_sort (out, by_start);
  return out;
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

/* how many showings of a series come before this one */
static int
showings_before (const CalEvent *ev, gint64 occ_start)
{
  int n = 0;
  for (guint k = 0, i = 0; i < 200000; k++, i++)
    {
      gint64 s = series_start (ev, k);
      if (s < ev->start)
        continue;
      if (s >= occ_start)
        break;
      n++;
    }
  return n;
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
    add_time (&master->exceptions, occ_start);
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
      if (edited->exceptions == NULL)
        edited->exceptions = copy_array (master->exceptions, sizeof (gint64));
      if (edited->completed == NULL)
        edited->completed = copy_array (master->completed, sizeof (gint64));
      return calendar_update (cal, edited);
    }
  if (scope == CAL_SCOPE_ONE)
    {
      add_time (&master->exceptions, occ_start);
      edited->repeat = CAL_REPEAT_NONE;
      edited->until = 0;
      edited->count = 0;
      calendar_add (cal, edited);   /* saves */
      return TRUE;
    }
  /* this showing and the ones after it: the old series stops, a new one starts here */
  if (master->count)
    {
      int before = showings_before (master, occ_start);
      edited->count = master->count > before ? master->count - before : 0;
    }
  if (!truncate_before (master, occ_start))
    {
      g_autofree char *keep = g_strdup (id);
      calendar_remove (cal, keep);
    }
  calendar_add (cal, edited);
  return TRUE;
}

/* ---- free time ---------------------------------------------------------- */

GArray *
calendar_free_slots (Calendar *cal, gint64 from, gint64 to, int minutes, int hour_from, int hour_to)
{
  GArray *slots = g_array_new (FALSE, FALSE, sizeof (gint64));
  GArray *occ = calendar_occurrences (cal, from, to);
  for (gint64 day = calendar_day_start (from); day < to; day = calendar_day_next (day))
    {
      g_autoptr (GDateTime) d = g_date_time_new_from_unix_local (day);
      g_autoptr (GDateTime) a = g_date_time_new_local (g_date_time_get_year (d), g_date_time_get_month (d),
                                                        g_date_time_get_day_of_month (d), hour_from, 0, 0);
      g_autoptr (GDateTime) b = g_date_time_new_local (g_date_time_get_year (d), g_date_time_get_month (d),
                                                        g_date_time_get_day_of_month (d), MIN (hour_to, 23), hour_to >= 24 ? 59 : 0, 0);
      gint64 wa = MAX (g_date_time_to_unix (a), from), wb = MIN (g_date_time_to_unix (b), to);
      if (wb <= wa)
        continue;
      gint64 cursor = wa;
      /* occurrences are sorted by start, so one pass finds the gaps */
      for (guint i = 0; i < occ->len; i++)
        {
          const CalOccurrence *o = &g_array_index (occ, CalOccurrence, i);
          if (o->event->all_day || o->event->reminder || o->end <= wa || o->start >= wb)
            continue;
          if (!calendar_calendar_find (cal, o->event->calendar)->visible)
            continue;
          if (o->start - cursor >= (gint64) minutes * 60)
            {
              g_array_append_val (slots, cursor);
              gint64 e = o->start;
              g_array_append_val (slots, e);
            }
          cursor = MAX (cursor, o->end);
        }
      if (wb - cursor >= (gint64) minutes * 60)
        {
          g_array_append_val (slots, cursor);
          g_array_append_val (slots, wb);
        }
    }
  g_array_free (occ, TRUE);
  return slots;
}

/* ---- alerts ------------------------------------------------------------- */

gboolean
calendar_next_alert (Calendar *cal, gint64 after, CalAlert *out)
{
  /* alerts reach at most a week back, so showings starting within ten days are enough */
  GArray *occ = calendar_occurrences (cal, after, after + 10 * 86400);
  gboolean found = FALSE;
  for (guint i = 0; i < occ->len; i++)
    {
      const CalOccurrence *o = &g_array_index (occ, CalOccurrence, i);
      if (o->event->reminder && calendar_is_done (o->event, o->start))
        continue;
      for (guint k = 0; k < o->event->alerts->len; k++)
        {
          gint64 fire = o->start - (gint64) g_array_index (o->event->alerts, int, k) * 60;
          if (fire <= after || (found && fire >= out->fire))
            continue;
          out->event = o->event;
          out->fire = fire;
          out->start = o->start;
          found = TRUE;
        }
    }
  g_array_free (occ, TRUE);
  return found;
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
