#include "calendar-ics.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_EVENTS 20000

/* ---- lines -------------------------------------------------------------- */

/* "A line may be continued on the next line if that starts with a space or tab" */
static GPtrArray *
unfolded_lines (const char *text)
{
  GPtrArray *lines = g_ptr_array_new_with_free_func (g_free);
  GString *cur = g_string_new (NULL);
  const char *p = text;
  while (*p)
    {
      const char *eol = p;
      while (*eol && *eol != '\n')
        eol++;
      gsize len = (gsize) (eol - p);
      if (len && p[len - 1] == '\r')
        len--;
      if (len && (*p == ' ' || *p == '\t') && cur->len)
        g_string_append_len (cur, p + 1, (gssize) len - 1);
      else
        {
          if (cur->len)
            g_ptr_array_add (lines, g_strdup (cur->str));
          g_string_assign (cur, "");
          g_string_append_len (cur, p, (gssize) len);
        }
      p = *eol ? eol + 1 : eol;
    }
  if (cur->len)
    g_ptr_array_add (lines, g_strdup (cur->str));
  g_string_free (cur, TRUE);
  return lines;
}

typedef struct {
  char *name;      /* upper case */
  char *params;    /* everything between the name and the colon, e.g. "TZID=Europe/Madrid" */
  char *value;
} Prop;

static void
prop_clear (Prop *p)
{
  g_free (p->name);
  g_free (p->params);
  g_free (p->value);
}

static gboolean
split_line (const char *line, Prop *p)
{
  /* the colon that ends the name and parameters: not one inside a quoted parameter value */
  gboolean quoted = FALSE;
  const char *colon = NULL;
  for (const char *c = line; *c; c++)
    {
      if (*c == '"')
        quoted = !quoted;
      else if (*c == ':' && !quoted)
        {
          colon = c;
          break;
        }
    }
  if (!colon)
    return FALSE;
  const char *semi = line;
  while (semi < colon && *semi != ';')
    semi++;
  g_autofree char *raw_name = g_strndup (line, (gsize) (semi - line));
  p->name = g_ascii_strup (raw_name, -1);
  p->params = semi < colon ? g_strndup (semi + 1, (gsize) (colon - semi - 1)) : g_strdup ("");
  p->value = g_strdup (colon + 1);
  return TRUE;
}

static char *
param (const char *params, const char *key)
{
  g_auto (GStrv) parts = g_strsplit (params, ";", -1);
  gsize klen = strlen (key);
  for (int i = 0; parts[i]; i++)
    if (g_ascii_strncasecmp (parts[i], key, klen) == 0 && parts[i][klen] == '=')
      {
        char *v = g_strdup (parts[i] + klen + 1);
        if (*v == '"')
          {
            char *t = g_strndup (v + 1, strlen (v) > 1 ? strlen (v) - 2 : 0);
            g_free (v);
            return t;
          }
        return v;
      }
  return NULL;
}

static char *
unescape_text (const char *v)
{
  GString *s = g_string_new (NULL);
  for (const char *c = v; *c; c++)
    {
      if (*c == '\\' && c[1])
        {
          c++;
          g_string_append_c (s, (*c == 'n' || *c == 'N') ? '\n' : *c);
        }
      else
        g_string_append_c (s, *c);
    }
  return g_string_free (s, FALSE);
}

/* ---- dates -------------------------------------------------------------- */

/* "20261009", "20261009T153000", "20261009T153000Z". tzid may be NULL for local time. */
static gboolean
parse_date_value (const char *v, const char *tzid, gint64 *out, gboolean *date_only)
{
  int y, mo, d, h = 0, mi = 0, sec = 0;
  int n = sscanf (v, "%4d%2d%2dT%2d%2d%2d", &y, &mo, &d, &h, &mi, &sec);
  if (n != 3 && n < 5)
    return FALSE;
  gboolean utc = strchr (v, 'Z') != NULL;
  if (date_only)
    *date_only = n == 3;
  g_autoptr (GTimeZone) tz = NULL;
  if (utc)
    tz = g_time_zone_new_utc ();
  else if (tzid && *tzid && n >= 5)
    {
      tz = g_time_zone_new_identifier (tzid);
      if (!tz)
        tz = g_time_zone_new_local ();
    }
  else
    tz = g_time_zone_new_local ();
  g_autoptr (GDateTime) dt = g_date_time_new (tz, y, mo, d, n >= 5 ? h : 0, n >= 5 ? mi : 0, n >= 6 ? sec : 0);
  if (!dt)
    return FALSE;
  /* all-day dates are local midnights whatever the file says */
  if (n == 3)
    {
      g_autoptr (GDateTime) local = g_date_time_new_local (y, mo, d, 0, 0, 0);
      if (!local)
        return FALSE;
      *out = g_date_time_to_unix (local);
    }
  else
    *out = g_date_time_to_unix (dt);
  return TRUE;
}

/* "PT15M", "P1D", "-PT1H30M", "P1W" in seconds; FALSE when not a duration */
static gboolean
parse_duration (const char *v, gint64 *seconds)
{
  const char *p = v;
  int sign = 1;
  if (*p == '-')
    {
      sign = -1;
      p++;
    }
  else if (*p == '+')
    p++;
  if (*p != 'P')
    return FALSE;
  p++;
  gboolean in_time = FALSE;
  gint64 total = 0;
  while (*p)
    {
      if (*p == 'T')
        {
          in_time = TRUE;
          p++;
          continue;
        }
      char *end;
      long n = strtol (p, &end, 10);
      if (end == p)
        return FALSE;
      switch (*end)
        {
        case 'W': total += n * 7 * 86400; break;
        case 'D': total += n * 86400; break;
        case 'H': total += n * 3600; break;
        case 'M': total += in_time ? n * 60 : 0; break;
        case 'S': total += n; break;
        default:  return FALSE;
        }
      p = end + 1;
    }
  *seconds = sign * total;
  return TRUE;
}

/* ---- an event as it is read --------------------------------------------- */

typedef struct {
  CalEvent *ev;
  char     *uid;
  gint64    recurrence_id;  /* 0 unless this is a changed single showing of a repeating event */
  gboolean  has_end;
  gboolean  cancelled;
} Item;

static int
weekday_bit (const char *two)
{
  static const char *names[] = { "MO", "TU", "WE", "TH", "FR", "SA", "SU" };
  for (int i = 0; i < 7; i++)
    if (g_ascii_strncasecmp (two, names[i], 2) == 0)
      return i;
  return -1;
}

static void
apply_rrule (CalEvent *ev, const char *rule)
{
  g_auto (GStrv) parts = g_strsplit (rule, ";", -1);
  const char *byday = NULL, *bymonthday = NULL;
  for (int i = 0; parts[i]; i++)
    {
      char *eq = strchr (parts[i], '=');
      if (!eq)
        continue;
      *eq = 0;
      const char *k = parts[i], *v = eq + 1;
      if (g_ascii_strcasecmp (k, "FREQ") == 0)
        {
          if (g_ascii_strcasecmp (v, "DAILY") == 0)
            ev->repeat = CAL_REPEAT_DAILY;
          else if (g_ascii_strcasecmp (v, "WEEKLY") == 0)
            ev->repeat = CAL_REPEAT_WEEKLY;
          else if (g_ascii_strcasecmp (v, "MONTHLY") == 0)
            ev->repeat = CAL_REPEAT_MONTHLY;
          else if (g_ascii_strcasecmp (v, "YEARLY") == 0)
            ev->repeat = CAL_REPEAT_YEARLY;
        }
      else if (g_ascii_strcasecmp (k, "INTERVAL") == 0)
        {
          long n = strtol (v, NULL, 10);
          ev->interval = n > 1 && n < 1000 ? (guint) n : 0;
        }
      else if (g_ascii_strcasecmp (k, "COUNT") == 0)
        {
          long n = strtol (v, NULL, 10);
          ev->count = n > 0 && n < 100000 ? (int) n : 0;
        }
      else if (g_ascii_strcasecmp (k, "UNTIL") == 0)
        {
          gint64 t;
          if (parse_date_value (v, NULL, &t, NULL))
            ev->until = calendar_day_start (t);
        }
      else if (g_ascii_strcasecmp (k, "BYDAY") == 0)
        byday = v;
      else if (g_ascii_strcasecmp (k, "BYMONTHDAY") == 0)
        bymonthday = v;
    }
  if (ev->repeat == CAL_REPEAT_WEEKLY && byday)
    {
      g_auto (GStrv) days = g_strsplit (byday, ",", -1);
      guint mask = 0;
      for (int i = 0; days[i]; i++)
        {
          const char *d = days[i];
          while (*d && (g_ascii_isdigit (*d) || *d == '-' || *d == '+'))
            d++;
          int b = weekday_bit (d);
          if (b >= 0)
            mask |= 1u << b;
        }
      ev->weekdays = mask;
    }
  else if (ev->repeat == CAL_REPEAT_MONTHLY)
    {
      if (byday && (g_ascii_isdigit (*byday) || *byday == '-' || *byday == '+'))
        ev->monthly = CAL_MONTHLY_WEEKDAY;     /* 2TU, -1FR */
      else if (bymonthday && g_str_has_prefix (bymonthday, "-1"))
        ev->monthly = CAL_MONTHLY_LAST_DAY;
    }
}

static void
add_exception_once (CalEvent *ev, gint64 t)
{
  if (!ev->exceptions)
    ev->exceptions = g_array_new (FALSE, FALSE, sizeof (gint64));
  for (guint i = 0; i < ev->exceptions->len; i++)
    if (g_array_index (ev->exceptions, gint64, i) == t)
      return;
  g_array_append_val (ev->exceptions, t);
}

static void
add_exdates (CalEvent *ev, const Prop *p, gboolean all_day)
{
  g_autofree char *tzid = param (p->params, "TZID");
  g_auto (GStrv) values = g_strsplit (p->value, ",", -1);
  for (int i = 0; values[i]; i++)
    {
      gint64 t;
      gboolean date_only;
      if (!parse_date_value (values[i], tzid, &t, &date_only))
        continue;
      (void) all_day;
      add_exception_once (ev, t);
    }
}

/* An alarm: "-PT15M" is 15 minutes before the start, "PT9H" is 9 hours after it (what Apple uses
 * for all-day events), and a fixed date and time is kept until the event's start is known. */
static void
add_alarm_trigger (CalEvent *ev, const Prop *p, GArray *absolute)
{
  g_autofree char *related = param (p->params, "RELATED");
  g_autofree char *kind = param (p->params, "VALUE");
  gint64 secs;
  if (related && g_ascii_strcasecmp (related, "END") == 0)
    return;
  if (kind && g_ascii_strcasecmp (kind, "DATE-TIME") == 0)
    {
      gint64 t;
      if (parse_date_value (p->value, NULL, &t, NULL))
        g_array_append_val (absolute, t);
      return;
    }
  if (!parse_duration (p->value, &secs) || secs > 4 * 7 * 86400 || -secs > 4 * 7 * 86400)
    return;
  int minutes = (int) (-secs / 60);       /* before the start is positive */
  if (minutes == -1)
    minutes = -2;                         /* -1 is the "none" marker */
  calendar_event_add_alert (ev, minutes);
}

static Item *
read_item (GPtrArray *lines, guint *i, gboolean todo)
{
  Item *item = g_new0 (Item, 1);
  CalEvent *ev = calendar_event_new ("", 0, 0, FALSE);
  item->ev = ev;
  ev->reminder = todo;
  gboolean have_start = FALSE, have_due = FALSE;
  gint64 duration = 0;
  gboolean in_alarm = FALSE;
  const char *end_marker = todo ? "END:VTODO" : "END:VEVENT";
  gint64 end_t = 0;
  g_autoptr (GArray) absolute_alerts = g_array_new (FALSE, FALSE, sizeof (gint64));

  for ((*i)++; *i < lines->len; (*i)++)
    {
      const char *line = lines->pdata[*i];
      if (g_ascii_strcasecmp (line, end_marker) == 0)
        break;
      if (g_ascii_strcasecmp (line, "BEGIN:VALARM") == 0)
        {
          in_alarm = TRUE;
          continue;
        }
      if (g_ascii_strcasecmp (line, "END:VALARM") == 0)
        {
          in_alarm = FALSE;
          continue;
        }
      Prop p = { 0 };
      if (!split_line (line, &p))
        continue;
      if (in_alarm)
        {
          if (g_str_equal (p.name, "TRIGGER"))
            add_alarm_trigger (ev, &p, absolute_alerts);
        }
      else if (g_str_equal (p.name, "SUMMARY"))
        {
          g_free (ev->title);
          ev->title = unescape_text (p.value);
        }
      else if (g_str_equal (p.name, "DESCRIPTION"))
        {
          g_free (ev->notes);
          ev->notes = unescape_text (p.value);
        }
      else if (g_str_equal (p.name, "LOCATION"))
        {
          g_free (ev->location);
          ev->location = unescape_text (p.value);
        }
      else if (g_str_equal (p.name, "URL"))
        {
          g_free (ev->url);
          ev->url = g_strdup (p.value);
        }
      else if (g_str_equal (p.name, "UID"))
        {
          g_free (item->uid);
          item->uid = g_strdup (p.value);
        }
      else if (g_str_equal (p.name, "STATUS"))
        {
          if (g_ascii_strcasecmp (p.value, "CANCELLED") == 0)
            item->cancelled = TRUE;
          else if (todo && g_ascii_strcasecmp (p.value, "COMPLETED") == 0)
            ev->done = TRUE;
        }
      else if (g_str_equal (p.name, "DTSTART") || g_str_equal (p.name, "DUE") || g_str_equal (p.name, "DTEND"))
        {
          g_autofree char *tzid = param (p.params, "TZID");
          gint64 t;
          gboolean date_only = FALSE;
          if (parse_date_value (p.value, tzid, &t, &date_only))
            {
              if (g_str_equal (p.name, "DTSTART"))
                {
                  ev->start = t;
                  ev->all_day = date_only;
                  have_start = TRUE;
                }
              else if (g_str_equal (p.name, "DUE"))
                {
                  end_t = t;
                  have_due = TRUE;
                }
              else
                {
                  end_t = t;
                  item->has_end = TRUE;
                }
            }
        }
      else if (g_str_equal (p.name, "DURATION"))
        parse_duration (p.value, &duration);
      else if (g_str_equal (p.name, "X-CALENDAR-REMINDER"))
        ev->reminder = g_ascii_strcasecmp (p.value, "TRUE") == 0;
      else if (g_str_equal (p.name, "X-CALENDAR-DONE"))
        ev->done = g_ascii_strcasecmp (p.value, "TRUE") == 0;
      else if (g_str_equal (p.name, "RRULE"))
        apply_rrule (ev, p.value);
      else if (g_str_equal (p.name, "EXDATE"))
        add_exdates (ev, &p, FALSE);
      else if (g_str_equal (p.name, "RECURRENCE-ID"))
        {
          g_autofree char *tzid = param (p.params, "TZID");
          parse_date_value (p.value, tzid, &item->recurrence_id, NULL);
        }
      prop_clear (&p);
    }

  /* an alarm at a fixed moment becomes "so many minutes before", unless the event repeats */
  for (guint k = 0; have_start && ev->repeat == CAL_REPEAT_NONE && k < absolute_alerts->len; k++)
    {
      gint64 before = (ev->start - g_array_index (absolute_alerts, gint64, k)) / 60;
      if (before > -4 * 7 * 1440 && before < 4 * 7 * 1440)
        calendar_event_add_alert (ev, (int) (before == -1 ? -2 : before));
    }

  if (!have_start && have_due)
    {
      ev->start = end_t;                 /* a to-do with only a due date */
      have_start = TRUE;
    }
  if (!have_start)
    {
      calendar_event_free (ev);
      g_free (item->uid);
      g_free (item);
      return NULL;
    }
  if (todo || ev->reminder)
    ev->end = ev->all_day ? calendar_day_next (ev->start) : ev->start + 900;
  else if (item->has_end && end_t > ev->start)
    ev->end = end_t;
  else if (duration > 0)
    ev->end = ev->start + duration;
  else
    ev->end = ev->all_day ? calendar_day_next (ev->start) : ev->start + (item->has_end ? 0 : 3600);
  if (ev->end <= ev->start)
    ev->end = ev->all_day ? calendar_day_next (ev->start) : ev->start + 900;
  /* a ten-year event is almost surely a mistake in the file */
  if (ev->end - ev->start > 3660LL * 86400)
    ev->end = ev->all_day ? calendar_day_next (ev->start) : ev->start + 3600;
  if (!*ev->title)
    {
      g_free (ev->title);
      ev->title = g_strdup ("(untitled)");
    }
  g_free (ev->uid);
  ev->uid = g_strdup (item->uid);
  return item;
}

static void
item_free (Item *it)
{
  g_free (it->uid);
  g_free (it);
}

GPtrArray *
calendar_ics_parse (const char *text)
{
  GPtrArray *events = g_ptr_array_new_with_free_func ((GDestroyNotify) calendar_event_free);
  if (!text)
    return events;
  g_autoptr (GPtrArray) lines = unfolded_lines (text);
  g_autoptr (GPtrArray) items = g_ptr_array_new_with_free_func ((GDestroyNotify) item_free);
  for (guint i = 0; i < lines->len && items->len < MAX_EVENTS; i++)
    {
      const char *line = lines->pdata[i];
      gboolean todo = g_ascii_strcasecmp (line, "BEGIN:VTODO") == 0;
      if (!todo && g_ascii_strcasecmp (line, "BEGIN:VEVENT") != 0)
        continue;
      Item *it = read_item (lines, &i, todo);
      if (!it)
        continue;
      if (it->cancelled)
        {
          calendar_event_free (it->ev);
          it->ev = NULL;
          item_free (it);
          continue;
        }
      g_ptr_array_add (items, it);
    }

  /* a changed single showing replaces that showing of its series */
  for (guint i = 0; i < items->len; i++)
    {
      Item *it = items->pdata[i];
      if (!it->recurrence_id || !it->uid)
        continue;
      for (guint k = 0; k < items->len; k++)
        {
          Item *master = items->pdata[k];
          if (master != it && !master->recurrence_id && master->uid && g_str_equal (master->uid, it->uid) &&
              master->ev->repeat != CAL_REPEAT_NONE)
            {
              add_exception_once (master->ev, it->recurrence_id);
              break;
            }
        }
    }
  for (guint i = 0; i < items->len; i++)
    {
      Item *it = items->pdata[i];
      it->ev->recurrence_id = it->recurrence_id;
      g_ptr_array_add (events, it->ev);
      it->ev = NULL;
    }
  return events;
}

/* ---- writing ------------------------------------------------------------ */

static void
put_line (GString *out, const char *line)
{
  /* lines are folded at 75 bytes, never inside a UTF-8 character */
  gsize len = strlen (line), pos = 0;
  gboolean first = TRUE;
  while (pos < len)
    {
      gsize room = first ? 75 : 74, take = MIN (room, len - pos);
      while (take < len - pos && take > 0 && ((guchar) line[pos + take] & 0xC0) == 0x80)
        take--;
      if (!first)
        g_string_append (out, "\r\n ");
      g_string_append_len (out, line + pos, (gssize) take);
      pos += take;
      first = FALSE;
    }
  g_string_append (out, "\r\n");
}

static char *
escape_text (const char *v)
{
  GString *s = g_string_new (NULL);
  for (const char *c = v; *c; c++)
    {
      if (*c == '\n')
        g_string_append (s, "\\n");
      else if (*c == '\r')
        continue;
      else
        {
          if (*c == '\\' || *c == ';' || *c == ',')
            g_string_append_c (s, '\\');
          g_string_append_c (s, *c);
        }
    }
  return g_string_free (s, FALSE);
}

static char *
format_utc (gint64 t)
{
  g_autoptr (GDateTime) d = g_date_time_new_from_unix_utc (t);
  return g_date_time_format (d, "%Y%m%dT%H%M%SZ");
}

static char *
format_day_value (gint64 t)
{
  g_autoptr (GDateTime) d = g_date_time_new_from_unix_local (t);
  return g_date_time_format (d, "%Y%m%d");
}

static char *
format_floating (gint64 t)
{
  g_autoptr (GDateTime) d = g_date_time_new_from_unix_local (t);
  return g_date_time_format (d, "%Y%m%dT%H%M%S");
}

static void
put_prop_text (GString *out, const char *name, const char *value)
{
  if (!value || !*value)
    return;
  g_autofree char *esc = escape_text (value);
  g_autofree char *line = g_strdup_printf ("%s:%s", name, esc);
  put_line (out, line);
}

/* server: reminders as marked events. floating: times without a zone, for series. */
static void
put_event (GString *out, const CalEvent *e, const char *stamp, gboolean server, gboolean floating)
{
  gboolean as_todo = e->reminder && !server;
  const char *kind = as_todo ? "VTODO" : "VEVENT";
  g_autofree char *begin = g_strdup_printf ("BEGIN:%s", kind);
  put_line (out, begin);
  g_autofree char *uid = g_strdup_printf ("UID:%s", e->uid ? e->uid : e->id);
  put_line (out, uid);
  g_autofree char *dtstamp = g_strdup_printf ("DTSTAMP:%s", stamp);
  put_line (out, dtstamp);
#define WHEN(t) (floating ? format_floating (t) : format_utc (t))
  if (e->recurrence_id)
    {
      g_autofree char *r = e->all_day ? format_day_value (e->recurrence_id) : WHEN (e->recurrence_id);
      g_autofree char *line = g_strdup_printf (e->all_day ? "RECURRENCE-ID;VALUE=DATE:%s" : "RECURRENCE-ID:%s", r);
      put_line (out, line);
    }
  if (e->all_day)
    {
      g_autofree char *a = format_day_value (e->start);
      g_autofree char *line = g_strdup_printf ("DTSTART;VALUE=DATE:%s", a);
      put_line (out, line);
      if (!as_todo)
        {
          g_autofree char *b = format_day_value (e->end);
          g_autofree char *line2 = g_strdup_printf ("DTEND;VALUE=DATE:%s", b);
          put_line (out, line2);
        }
    }
  else
    {
      g_autofree char *a = WHEN (e->start);
      g_autofree char *line = g_strdup_printf ("DTSTART:%s", a);
      put_line (out, line);
      if (!as_todo)
        {
          g_autofree char *b = WHEN (e->end);
          g_autofree char *line2 = g_strdup_printf ("DTEND:%s", b);
          put_line (out, line2);
        }
    }
  put_prop_text (out, "SUMMARY", e->title);
  put_prop_text (out, "LOCATION", e->location);
  put_prop_text (out, "DESCRIPTION", e->notes);
  if (e->url && *e->url)
    {
      g_autofree char *line = g_strdup_printf ("URL:%s", e->url);
      put_line (out, line);
    }
  if (e->reminder && server)
    {
      put_line (out, "X-CALENDAR-REMINDER:TRUE");
      if (e->done && e->repeat == CAL_REPEAT_NONE)
        put_line (out, "X-CALENDAR-DONE:TRUE");
    }
  if (as_todo && e->done && e->repeat == CAL_REPEAT_NONE)
    put_line (out, "STATUS:COMPLETED");

  if (e->repeat != CAL_REPEAT_NONE)
    {
      static const char *freq[] = { "", "DAILY", "WEEKLY", "MONTHLY", "YEARLY" };
      static const char *dow[] = { "MO", "TU", "WE", "TH", "FR", "SA", "SU" };
      GString *r = g_string_new (NULL);
      g_string_append_printf (r, "RRULE:FREQ=%s", freq[e->repeat]);
      if (e->interval > 1)
        g_string_append_printf (r, ";INTERVAL=%u", e->interval);
      if (e->repeat == CAL_REPEAT_WEEKLY && e->weekdays)
        {
          g_string_append (r, ";BYDAY=");
          gboolean first = TRUE;
          for (int i = 0; i < 7; i++)
            if ((e->weekdays >> i) & 1)
              {
                g_string_append_printf (r, "%s%s", first ? "" : ",", dow[i]);
                first = FALSE;
              }
        }
      if (e->repeat == CAL_REPEAT_MONTHLY && e->monthly == CAL_MONTHLY_LAST_DAY)
        g_string_append (r, ";BYMONTHDAY=-1");
      if (e->repeat == CAL_REPEAT_MONTHLY && e->monthly == CAL_MONTHLY_WEEKDAY)
        {
          g_autoptr (GDateTime) d = g_date_time_new_from_unix_local (e->start);
          int nth = (g_date_time_get_day_of_month (d) - 1) / 7 + 1;
          g_string_append_printf (r, ";BYDAY=%d%s", nth == 5 ? -1 : nth, dow[g_date_time_get_day_of_week (d) - 1]);
        }
      if (e->count)
        g_string_append_printf (r, ";COUNT=%d", e->count);
      else if (e->until)
        {
          g_autofree char *u = format_day_value (e->until);
          g_string_append_printf (r, ";UNTIL=%s", u);
        }
      put_line (out, r->str);
      g_string_free (r, TRUE);
      for (guint i = 0; e->exceptions && i < e->exceptions->len; i++)
        {
          gint64 t = g_array_index (e->exceptions, gint64, i);
          g_autofree char *x = e->all_day ? format_day_value (t) : WHEN (t);
          g_autofree char *line = g_strdup_printf (e->all_day ? "EXDATE;VALUE=DATE:%s" : "EXDATE:%s", x);
          put_line (out, line);
        }
    }
  for (guint i = 0; i < e->alerts->len; i++)
    {
      int m = g_array_index (e->alerts, int, i);
      put_line (out, "BEGIN:VALARM");
      put_line (out, "ACTION:DISPLAY");
      put_prop_text (out, "DESCRIPTION", e->title && *e->title ? e->title : "Reminder");
      g_autofree char *trigger = m >= 0 ? g_strdup_printf ("TRIGGER:-PT%dM", m) : g_strdup_printf ("TRIGGER:PT%dM", -m);
      put_line (out, trigger);
      put_line (out, "END:VALARM");
    }
#undef WHEN
  g_autofree char *end = g_strdup_printf ("END:%s", kind);
  put_line (out, end);
}

/* the events of a resource are written together: a series and its changed showings */
static void
put_group (GString *out, const GPtrArray *group, const char *stamp, gboolean server)
{
  gboolean floating = FALSE;
  for (guint i = 0; i < group->len; i++)
    {
      const CalEvent *e = group->pdata[i];
      floating |= e->repeat != CAL_REPEAT_NONE && !e->all_day;
    }
  /* the series first, then its changed showings */
  for (guint pass = 0; pass < 2; pass++)
    for (guint i = 0; i < group->len; i++)
      {
        const CalEvent *e = group->pdata[i];
        if ((pass == 0) == (e->recurrence_id == 0))
          put_event (out, e, stamp, server, floating);
      }
}

char *
calendar_ics_export_events (const GPtrArray *events)
{
  GString *out = g_string_new (NULL);
  put_line (out, "BEGIN:VCALENDAR");
  put_line (out, "VERSION:2.0");
  put_line (out, "PRODID:-//vezzulab//Calendar//EN");
  put_line (out, "CALSCALE:GREGORIAN");
  g_autofree char *stamp = format_utc (g_get_real_time () / G_USEC_PER_SEC);
  put_group (out, events, stamp, TRUE);
  put_line (out, "END:VCALENDAR");
  return g_string_free (out, FALSE);
}

char *
calendar_ics_export (Calendar *cal, const char *calendar_id)
{
  GString *out = g_string_new (NULL);
  put_line (out, "BEGIN:VCALENDAR");
  put_line (out, "VERSION:2.0");
  put_line (out, "PRODID:-//vezzulab//Calendar//EN");
  put_line (out, "CALSCALE:GREGORIAN");
  g_autofree char *stamp = format_utc (g_get_real_time () / G_USEC_PER_SEC);
  const GPtrArray *events = calendar_all_events (cal);
  /* events that share a UID are one series and its changed showings: write them together */
  g_autoptr (GHashTable) groups = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, (GDestroyNotify) g_ptr_array_unref);
  g_autoptr (GPtrArray) order = g_ptr_array_new ();
  for (guint i = 0; i < events->len; i++)
    {
      const CalEvent *e = events->pdata[i];
      if (calendar_id && !g_str_equal (e->calendar, calendar_id))
        continue;
      const char *key = e->uid ? e->uid : e->id;
      GPtrArray *g = g_hash_table_lookup (groups, key);
      if (!g)
        {
          g = g_ptr_array_new ();
          g_hash_table_insert (groups, (gpointer) key, g);
          g_ptr_array_add (order, (gpointer) key);
        }
      g_ptr_array_add (g, (gpointer) e);
    }
  for (guint i = 0; i < order->len; i++)
    put_group (out, g_hash_table_lookup (groups, order->pdata[i]), stamp, FALSE);
  put_line (out, "END:VCALENDAR");
  return g_string_free (out, FALSE);
}
