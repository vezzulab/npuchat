#include "calendar-quick.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  char **raw;    /* words as typed, trailing punctuation removed */
  char **norm;   /* lowercase, no accents */
  gboolean *used;
  guint     n;
} Words;

static const char *weekday_names[7][2] = {
  { "lunes", "monday" }, { "martes", "tuesday" }, { "miercoles", "wednesday" }, { "jueves", "thursday" },
  { "viernes", "friday" }, { "sabado", "saturday" }, { "domingo", "sunday" },
};
static const char *month_names[12][2] = {
  { "enero", "january" }, { "febrero", "february" }, { "marzo", "march" }, { "abril", "april" },
  { "mayo", "may" }, { "junio", "june" }, { "julio", "july" }, { "agosto", "august" },
  { "septiembre", "september" }, { "octubre", "october" }, { "noviembre", "november" }, { "diciembre", "december" },
};

static void
words_free (Words *w)
{
  g_strfreev (w->raw);
  g_strfreev (w->norm);
  g_free (w->used);
}

static void
words_init (Words *w, const char *text)
{
  g_autofree char *ascii = g_str_to_ascii (text, NULL);
  g_autofree char *lower = g_ascii_strdown (ascii, -1);
  g_auto (GStrv) rawv = g_strsplit_set (text, " \t\n", -1);
  g_auto (GStrv) lowv = g_strsplit_set (lower, " \t\n", -1);
  GPtrArray *r = g_ptr_array_new (), *l = g_ptr_array_new ();
  /* the transliteration can change word boundaries only by length, never by count of spaces */
  for (guint i = 0; rawv[i] && lowv[i]; i++)
    {
      if (!*rawv[i])
        continue;
      g_autofree char *tail = g_strdup (lowv[i]);
      size_t len = strlen (tail);
      while (len && strchr (",.;:!?", tail[len - 1]) && !(len >= 2 && isdigit ((unsigned char) tail[len - 2]) && tail[len - 1] == '.'))
        tail[--len] = 0;
      if (!len)
        continue;
      g_autofree char *rtail = g_strdup (rawv[i]);
      size_t rlen = strlen (rtail);
      while (rlen && strchr (",.;:!?", rtail[rlen - 1]))
        rtail[--rlen] = 0;
      g_ptr_array_add (r, g_strdup (rtail));
      g_ptr_array_add (l, g_strdup (tail));
    }
  w->n = r->len;
  g_ptr_array_add (r, NULL);
  g_ptr_array_add (l, NULL);
  w->raw = (char **) g_ptr_array_free (r, FALSE);
  w->norm = (char **) g_ptr_array_free (l, FALSE);
  w->used = g_new0 (gboolean, w->n + 1);
}

static gboolean
is (const Words *w, guint i, const char *a, const char *b, const char *c, const char *d)
{
  if (i >= w->n || w->used[i])
    return FALSE;
  const char *t = w->norm[i];
  return (a && g_str_equal (t, a)) || (b && g_str_equal (t, b)) || (c && g_str_equal (t, c)) || (d && g_str_equal (t, d));
}

static void
use (Words *w, guint from, guint count)
{
  for (guint i = from; i < from + count && i < w->n; i++)
    w->used[i] = TRUE;
}

static int
weekday_of (const char *t)
{
  for (int i = 0; i < 7; i++)
    if (g_str_equal (t, weekday_names[i][0]) || g_str_equal (t, weekday_names[i][1]))
      return i;
  /* short forms */
  static const char *shorts[7][2] = { { "lun", "mon" }, { "mar", "tue" }, { "mie", "wed" }, { "jue", "thu" },
                                      { "vie", "fri" }, { "sab", "sat" }, { "dom", "sun" } };
  for (int i = 0; i < 7; i++)
    if (g_str_equal (t, shorts[i][0]) || g_str_equal (t, shorts[i][1]))
      return i;
  return -1;
}

static int
month_of (const char *t)
{
  for (int i = 0; i < 12; i++)
    if (g_str_equal (t, month_names[i][0]) || g_str_equal (t, month_names[i][1]))
      return i + 1;
  if (strlen (t) == 3)
    for (int i = 0; i < 12; i++)
      if (g_str_has_prefix (month_names[i][0], t) || g_str_has_prefix (month_names[i][1], t))
        return i + 1;
  return 0;
}

/* "3", "tres", "three" */
static int
number_word (const char *t)
{
  static const char *words[][2] = { { "un", "one" }, { "una", "a" }, { "dos", "two" }, { "tres", "three" }, { "cuatro", "four" },
                                    { "cinco", "five" }, { "seis", "six" }, { "siete", "seven" }, { "ocho", "eight" },
                                    { "nueve", "nine" }, { "diez", "ten" } };
  static const int values[] = { 1, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
  for (guint i = 0; i < G_N_ELEMENTS (words); i++)
    if (g_str_equal (t, words[i][0]) || g_str_equal (t, words[i][1]))
      return values[i];
  return 0;
}

static gboolean
all_digits (const char *t, int max_len)
{
  size_t n = strlen (t);
  if (!n || (int) n > max_len)
    return FALSE;
  for (size_t i = 0; i < n; i++)
    if (!isdigit ((unsigned char) t[i]))
      return FALSE;
  return TRUE;
}

/* ---- clocks ------------------------------------------------------------- */

typedef struct {
  int h, m;
  int ampm;       /* 0 none, 1 am, 2 pm */
  gboolean sure;  /* has ":" or am/pm, so it cannot be a plain number */
} Clock;

/* "7", "7pm", "7:30", "7:30pm", "19:00" */
static gboolean
parse_clock (const char *t, Clock *c)
{
  int h, m = 0;
  char suffix[4] = "";
  int n = sscanf (t, "%2d:%2d%3s", &h, &m, suffix);
  if (n >= 2)
    c->sure = TRUE;
  else
    {
      m = 0;
      n = sscanf (t, "%2d%3s", &h, suffix);
      if (n < 1)
        return FALSE;
      c->sure = FALSE;
    }
  /* nothing may follow the number except am/pm */
  const char *p = t;
  while (isdigit ((unsigned char) *p) || *p == ':')
    p++;
  c->ampm = 0;
  if (g_str_equal (p, "am") || g_str_equal (p, "a.m.") || g_str_equal (p, "a.m"))
    c->ampm = 1;
  else if (g_str_equal (p, "pm") || g_str_equal (p, "p.m.") || g_str_equal (p, "p.m"))
    c->ampm = 2;
  else if (*p)
    return FALSE;
  if (c->ampm)
    c->sure = TRUE;
  if (h < 0 || h > 24 || m < 0 || m > 59 || (c->ampm && (h < 1 || h > 12)))
    return FALSE;
  c->h = h;
  c->m = m;
  return TRUE;
}

/* 24-hour hour of a clock. A bare "7" is read as evening, "9" as morning. */
static int
clock_hour (const Clock *c, int ampm_hint)
{
  int ampm = c->ampm ? c->ampm : ampm_hint;
  if (ampm == 1)
    return c->h % 12;
  if (ampm == 2)
    return c->h % 12 + 12;
  if (c->h >= 13 || c->h == 0 || c->sure)
    return c->h % 24;
  if (c->h == 12)
    return 12;
  return c->h <= 7 ? c->h + 12 : c->h;
}

/* "de la manana/tarde/noche", "in the morning/afternoon/evening", "at night" after word i */
static int
ampm_words_mode (Words *w, guint i, gboolean consume)
{
  guint k = i;
  if (is (w, k, "de", "in", "at", NULL) && is (w, k + 1, "la", "the", NULL, NULL))
    k += 2;
  else if (is (w, k, "de", "at", NULL, NULL))
    k += 1;
  else if (is (w, k, "por", NULL, NULL, NULL) && is (w, k + 1, "la", NULL, NULL, NULL))
    k += 2;
  else
    return 0;
  int r = 0;
  if (is (w, k, "manana", "morning", NULL, NULL))
    r = 1;
  else if (is (w, k, "tarde", "afternoon", "noche", "evening") || is (w, k, "night", NULL, NULL, NULL))
    r = 2;
  if (r && consume)
    use (w, i, k + 1 - i);
  return r;
}

static int
ampm_words (Words *w, guint i)
{
  return ampm_words_mode (w, i, TRUE);
}

/* ---- the parser --------------------------------------------------------- */

static gint64
at_midnight (int y, int mo, int d)
{
  g_autoptr (GDateTime) dt = g_date_time_new_local (y, mo, d, 0, 0, 0);
  return dt ? g_date_time_to_unix (dt) : 0;
}

static gint64
add_days (gint64 day, int n)
{
  g_autoptr (GDateTime) d = g_date_time_new_from_unix_local (day);
  g_autoptr (GDateTime) r = g_date_time_add_days (d, n);
  return g_date_time_to_unix (r);
}

static gint64
at_clock (gint64 day, int h, int m)
{
  g_autoptr (GDateTime) d = g_date_time_new_from_unix_local (day);
  g_autoptr (GDateTime) r = g_date_time_new_local (g_date_time_get_year (d), g_date_time_get_month (d),
                                                    g_date_time_get_day_of_month (d), h % 24, m, 0);
  return g_date_time_to_unix (r);
}

static int
dow_of (gint64 day)
{
  g_autoptr (GDateTime) d = g_date_time_new_from_unix_local (day);
  return g_date_time_get_day_of_week (d) - 1;
}

/* the next date with this day of month, today included */
static gint64
next_day_of_month (gint64 today, int dom)
{
  gint64 day = today;
  for (int i = 0; i < 400; i++, day = add_days (day, 1))
    {
      g_autoptr (GDateTime) d = g_date_time_new_from_unix_local (day);
      if (g_date_time_get_day_of_month (d) == dom)
        return day;
    }
  return today;
}

/* a month and day, in the coming year when it already passed */
static gint64
month_day (gint64 today, int month, int day, int year)
{
  g_autoptr (GDateTime) t = g_date_time_new_from_unix_local (today);
  int y = year ? year : g_date_time_get_year (t);
  gint64 r = at_midnight (y, month, day);
  if (!year && r < today)
    r = at_midnight (y + 1, month, day);
  return r;
}

void
calendar_quick_clear (CalQuick *q)
{
  g_free (q->title);
  q->title = NULL;
}

gboolean
calendar_quick_parse (const char *text, gint64 now, gboolean month_first, CalQuick *out)
{
  memset (out, 0, sizeof *out);
  out->alert = -1;
  if (!text || !*text)
    return FALSE;

  Words w;
  words_init (&w, text);
  gint64 today = calendar_day_start (now);
  gint64 day = 0;                 /* the chosen date, 0 = none yet */
  gboolean have_time = FALSE, have_end = FALSE;
  Clock start_c = { 0 }, end_c = { 0 };
  int hint_start = 0, hint_end = 0;
  int duration_min = 0;
  CalRepeat repeat = CAL_REPEAT_NONE;

  /* "remind me to…", "recuérdame…", "recordatorio": a task to tick off */
  for (guint i = 0; i < w.n; i++)
    {
      if (is (&w, i, "recuerdame", "recordatorio", "reminder", NULL) || (is (&w, i, "remind", NULL, NULL, NULL) && is (&w, i + 1, "me", NULL, NULL, NULL)))
        {
          out->reminder = TRUE;
          use (&w, i, is (&w, i, "remind", NULL, NULL, NULL) ? 2 : 1);
          if (is (&w, i + (is (&w, i, "remind", NULL, NULL, NULL) ? 2u : 1u), "to", "de", "que", NULL))
            use (&w, i + (w.used[i] && i + 1 < w.n && w.used[i + 1] ? 2u : 1u), 1);
        }
    }

  /* "15 min antes", "1 hour before", "avísame 2 horas antes": the alert */
  for (guint i = 0; i + 2 < w.n; i++)
    {
      if (w.used[i] || w.used[i + 1] || w.used[i + 2])
        continue;
      int n = all_digits (w.norm[i], 3) ? atoi (w.norm[i]) : number_word (w.norm[i]);
      if (!n || !is (&w, i + 2, "antes", "before", NULL, NULL))
        continue;
      int mult = 0;
      if (is (&w, i + 1, "min", "mins", "minuto", "minutos") || is (&w, i + 1, "minute", "minutes", NULL, NULL))
        mult = 1;
      else if (is (&w, i + 1, "h", "hr", "hora", "horas") || is (&w, i + 1, "hour", "hours", NULL, NULL))
        mult = 60;
      else if (is (&w, i + 1, "dia", "dias", "day", "days"))
        mult = 1440;
      else if (is (&w, i + 1, "semana", "semanas", "week", "weeks"))
        mult = 10080;
      if (!mult)
        continue;
      out->alert = n * mult;
      use (&w, i, 3);
      /* the words that led into it: "avísame", "con aviso de", "alert me" */
      for (guint back = 0; back < 3 && i > 0; back++)
        {
          guint k = i - 1;
          if (!w.used[k] && (is (&w, k, "avisame", "aviso", "alerta", "alert") || is (&w, k, "con", "de", "me", "notify")))
            {
              use (&w, k, 1);
              i = k;
            }
          else
            break;
        }
      break;
    }

  /* "hasta el 15 de diciembre", "until december 15", "5 veces", "5 times": where a repeat stops */
  for (guint i = 0; i < w.n; i++)
    {
      if (w.used[i])
        continue;
      if (is (&w, i, "hasta", "until", "through", NULL))
        {
          guint k = i + 1;
          if (is (&w, k, "el", "the", NULL, NULL))
            k++;
          gint64 stop = 0;
          guint len = 0;
          if (k + 2 < w.n && !w.used[k] && all_digits (w.norm[k], 2) && is (&w, k + 1, "de", NULL, NULL, NULL) && month_of (w.norm[k + 2]))
            {
              stop = month_day (today, month_of (w.norm[k + 2]), atoi (w.norm[k]), 0);
              len = k + 3 - i;
            }
          else if (k + 1 < w.n && !w.used[k] && month_of (w.norm[k]) && strlen (w.norm[k]) >= 3 && all_digits (w.norm[k + 1], 2))
            {
              stop = month_day (today, month_of (w.norm[k]), atoi (w.norm[k + 1]), 0);
              len = k + 2 - i;
            }
          else if (k < w.n && !w.used[k] && strchr (w.norm[k], '/'))
            {
              int a, b;
              if (sscanf (w.norm[k], "%d/%d", &a, &b) == 2)
                {
                  int dd = month_first ? b : a, mm = month_first ? a : b;
                  if (mm >= 1 && mm <= 12 && dd >= 1 && dd <= 31)
                    {
                      stop = month_day (today, mm, dd, 0);
                      len = k + 1 - i;
                    }
                }
            }
          if (stop)
            {
              out->until = stop;
              use (&w, i, len);
            }
        }
      else if ((all_digits (w.norm[i], 3) && is (&w, i + 1, "veces", "times", "vez", NULL)) )
        {
          out->count = atoi (w.norm[i]);
          use (&w, i, 2);
        }
    }

  /* "7 de la manana" must be read before "manana" means tomorrow, so times come first */
  for (guint i = 0; i < w.n; i++)
    {
      if (w.used[i])
        continue;
      guint at = i, skip_marker = 0;
      /* range "de 10 a 11", "from 3 to 4pm", "10-11", "10 a 11" */
      guint j = i;
      if (is (&w, j, "de", "from", "entre", NULL) && j + 1 < w.n)
        {
          j++;
          skip_marker = 1;
        }
      if (is (&w, j, "las", NULL, NULL, NULL))
        {
          j++;
          skip_marker++;
        }
      Clock a, b;
      const char *tok = j < w.n && !w.used[j] ? w.norm[j] : NULL;
      if (!tok)
        continue;
      const char *dash = strchr (tok, '-');
      if (dash && dash != tok && dash[1])
        {
          g_autofree char *left = g_strndup (tok, (gsize) (dash - tok));
          if (parse_clock (left, &a) && parse_clock (dash + 1, &b) && (a.sure || b.sure || a.h < 24))
            {
              if (!(!a.sure && !b.sure && (strlen (left) > 2 || strlen (dash + 1) > 2)))
                {
                  /* "10-11pm": the am/pm of the end applies to the start too */
                  int shared = b.ampm && !a.ampm ? b.ampm : 0;
                  start_c = a;
                  end_c = b;
                  hint_start = shared;
                  have_time = have_end = TRUE;
                  use (&w, at, skip_marker + 1);
                  int words_hint = ampm_words (&w, j + 1);
                  if (words_hint)
                    hint_start = hint_end = words_hint;
                  continue;
                }
            }
        }
      if (!parse_clock (tok, &a))
        continue;
      /* a bare number is a time only when a marker introduces it or it has am/pm or ":" */
      gboolean marker = skip_marker || (i > 0 && is (&w, i - 1, "las", "at", "a", NULL) && i - 1 < w.n && !w.used[i - 1]);
      guint marker_at = i;
      if (i > 0 && !w.used[i - 1] && is (&w, i - 1, "las", "at", NULL, NULL))
        {
          marker = TRUE;
          marker_at = i - 1;
          if (i > 1 && is (&w, i - 2, "a", NULL, NULL, NULL))
            marker_at = i - 2;
        }
      if (!a.sure && !marker && all_digits (tok, 2) && ampm_words_mode (&w, j + 1, FALSE))
        marker = TRUE;
      if (!a.sure && !marker)
        {
          /* "10 a 11" without markers: two bare numbers joined by a/to */
          Clock b2;
          if (all_digits (tok, 2) && (is (&w, j + 1, "a", "to", "-", NULL)) && j + 2 < w.n && parse_clock (w.norm[j + 2], &b2) && !w.used[j + 2] && (b2.sure || skip_marker))
            {
              start_c = a;
              end_c = b2;
              if (b2.ampm)
                hint_start = b2.ampm;
              have_time = have_end = TRUE;
              use (&w, at, skip_marker + 1);
              use (&w, j + 1, 2);
              continue;
            }
          continue;
        }
      if (marker_at < j && !skip_marker)
        use (&w, marker_at, j - marker_at);
      use (&w, j, 1);
      if (skip_marker)
        use (&w, at, skip_marker);
      if (!have_time)
        {
          start_c = a;
          have_time = TRUE;
          hint_start = ampm_words (&w, j + 1);
          /* "3pm to 4pm", "3 to 4pm", "7 a 9" */
          guint k = j + 1;
          while (k < w.n && w.used[k])
            k++;
          Clock b3;
          if (k + 1 < w.n && is (&w, k, "a", "to", "hasta", "until") && !w.used[k + 1] && parse_clock (w.norm[k + 1], &b3) &&
              (b3.sure || a.sure || skip_marker || marker))
            {
              end_c = b3;
              have_end = TRUE;
              use (&w, k, 2);
              hint_end = ampm_words (&w, k + 2);
              if (!hint_start && b3.ampm && !a.ampm)
                hint_start = b3.ampm;
            }
        }
    }
  /* "noon", "midnight" */
  for (guint i = 0; i < w.n; i++)
    {
      if (!have_time && is (&w, i, "mediodia", "noon", "midday", NULL))
        {
          start_c = (Clock) { 12, 0, 0, TRUE };
          have_time = TRUE;
          use (&w, i, 1);
        }
      else if (!have_time && is (&w, i, "medianoche", "midnight", NULL, NULL))
        {
          start_c = (Clock) { 0, 0, 0, TRUE };
          have_time = TRUE;
          use (&w, i, 1);
        }
    }

  /* durations: "por 2 horas", "for 90 minutes", "1h30", "2h" */
  for (guint i = 0; i < w.n; i++)
    {
      if (w.used[i])
        continue;
      int h = 0, m = 0;
      if (is (&w, i, "por", "for", "durante", NULL) && i + 2 < w.n && !w.used[i + 1] && all_digits (w.norm[i + 1], 3))
        {
          int n = atoi (w.norm[i + 1]);
          if (is (&w, i + 2, "hora", "horas", "hour", "hours") || is (&w, i + 2, "hr", "hrs", "h", NULL))
            duration_min = n * 60;
          else if (is (&w, i + 2, "minutos", "min", "minutes", "mins"))
            duration_min = n;
          else
            continue;
          use (&w, i, 3);
        }
      else if (sscanf (w.norm[i], "%dh%d%*[m]", &h, &m) >= 1 && strchr (w.norm[i], 'h') && h < 24)
        {
          char *p = strchr (w.norm[i], 'h');
          g_autofree char *hours = g_strndup (w.norm[i], (gsize) (p - w.norm[i]));
          if (all_digits (hours, 2) && (!p[1] || all_digits (p + 1, 2) || g_str_has_suffix (p, "m")))
            {
              duration_min = h * 60 + m;
              use (&w, i, 1);
            }
        }
    }

  /* repeat: "cada lunes", "todos los dias", "every week", "daily" */
  for (guint i = 0; i < w.n; i++)
    {
      if (w.used[i])
        continue;
      if (is (&w, i, "diario", "diaria", "daily", NULL))
        { repeat = CAL_REPEAT_DAILY; use (&w, i, 1); }
      else if (is (&w, i, "semanal", "weekly", NULL, NULL))
        { repeat = CAL_REPEAT_WEEKLY; use (&w, i, 1); }
      else if (is (&w, i, "mensual", "monthly", NULL, NULL))
        { repeat = CAL_REPEAT_MONTHLY; use (&w, i, 1); }
      else if (is (&w, i, "anual", "yearly", NULL, NULL))
        { repeat = CAL_REPEAT_YEARLY; use (&w, i, 1); }
      else if (is (&w, i, "cada", "every", NULL, NULL) || (is (&w, i, "todos", "todas", NULL, NULL) && is (&w, i + 1, "los", "las", NULL, NULL)))
        {
          guint k = i + (is (&w, i, "todos", "todas", NULL, NULL) ? 2 : 1);
          /* "cada 2 semanas", "every other week", "cada tres meses": a number before the unit */
          int step = 0;
          if (k < w.n && !w.used[k])
            {
              if (is (&w, k, "other", "otra", "otro", NULL))
                step = 2;
              else if (all_digits (w.norm[k], 2))
                step = atoi (w.norm[k]);
              else
                step = number_word (w.norm[k]);
              if (step)
                k++;        /* the unit comes next */
            }
          guint every = step > 1 ? (guint) step : 0;
          if (is (&w, k, "dia", "dias", "day", "days"))
            { repeat = CAL_REPEAT_DAILY; out->interval = every; use (&w, i, k + 1 - i); }
          else if (is (&w, k, "semana", "semanas", "week", "weeks"))
            { repeat = CAL_REPEAT_WEEKLY; out->interval = every; use (&w, i, k + 1 - i); }
          else if (is (&w, k, "mes", "meses", "month", "months"))
            { repeat = CAL_REPEAT_MONTHLY; out->interval = every; use (&w, i, k + 1 - i); }
          else if (is (&w, k, "ano", "anos", "year", "years"))
            { repeat = CAL_REPEAT_YEARLY; out->interval = every; use (&w, i, k + 1 - i); }
          else if (k < w.n && !w.used[k] && weekday_of (w.norm[k]) >= 0)
            {
              repeat = CAL_REPEAT_WEEKLY;
              day = add_days (today, (weekday_of (w.norm[k]) - dow_of (today) + 7) % 7);
              use (&w, i, k + 1 - i);
            }
        }
    }

  /* the date */
  for (guint i = 0; i < w.n && !day; i++)
    {
      if (w.used[i])
        continue;
      const char *t = w.norm[i];
      int n;
      if (is (&w, i, "pasado", NULL, NULL, NULL) && is (&w, i + 1, "manana", NULL, NULL, NULL))
        { day = add_days (today, 2); use (&w, i, 2); }
      else if (is (&w, i, "day", NULL, NULL, NULL) && is (&w, i + 1, "after", NULL, NULL, NULL) && is (&w, i + 2, "tomorrow", NULL, NULL, NULL))
        { day = add_days (today, 2); use (&w, i, 3); }
      else if (is (&w, i, "hoy", "today", "tonight", NULL))
        { day = today; use (&w, i, 1); }
      else if (is (&w, i, "esta", NULL, NULL, NULL) && is (&w, i + 1, "noche", NULL, NULL, NULL))
        { day = today; if (!have_time) { start_c = (Clock) { 8, 0, 2, TRUE }; have_time = TRUE; } use (&w, i, 2); }
      else if (is (&w, i, "manana", "tomorrow", NULL, NULL))
        { day = add_days (today, 1); use (&w, i, 1); }
      else if (is (&w, i, "en", "in", NULL, NULL) && i + 2 < w.n && !w.used[i + 1] && all_digits (w.norm[i + 1], 3) &&
               (is (&w, i + 2, "dias", "dia", "days", "day") || is (&w, i + 2, "semanas", "semana", "weeks", "week") ||
                is (&w, i + 2, "meses", "mes", "months", "month")))
        {
          n = atoi (w.norm[i + 1]);
          const char *u = w.norm[i + 2];
          g_autoptr (GDateTime) d = g_date_time_new_from_unix_local (today);
          g_autoptr (GDateTime) r = (u[0] == 'd') ? g_date_time_add_days (d, n)
                                    : (u[0] == 's' || u[0] == 'w') ? g_date_time_add_weeks (d, n)
                                                                    : g_date_time_add_months (d, n);
          day = g_date_time_to_unix (r);
          use (&w, i, 3);
        }
      else if ((is (&w, i, "proximo", "proxima", "next", "este") || is (&w, i, "esta", "this", NULL, NULL)) && i + 1 < w.n &&
               !w.used[i + 1] && weekday_of (w.norm[i + 1]) >= 0)
        {
          int target = weekday_of (w.norm[i + 1]);
          int diff = (target - dow_of (today) + 7) % 7;
          if (diff == 0 && !is (&w, i, "este", "esta", "this", NULL))
            diff = 7;
          day = add_days (today, diff);
          use (&w, i, 2);
        }
      else if (weekday_of (t) >= 0 && (strlen (t) > 3 || (i > 0 && is (&w, i - 1, "el", "on", NULL, NULL))))
        {
          int diff = (weekday_of (t) - dow_of (today) + 7) % 7;
          day = add_days (today, diff);
          use (&w, i, 1);
          if (i > 0 && is (&w, i - 1, "el", "on", "este", "this"))
            use (&w, i - 1, 1);
        }
      else if (all_digits (t, 2) && i + 2 < w.n && is (&w, i + 1, "de", NULL, NULL, NULL) && !w.used[i + 2] && month_of (w.norm[i + 2]))
        {
          int year = 0;
          guint len = 3;
          if (i + 4 < w.n && is (&w, i + 3, "de", NULL, NULL, NULL) && all_digits (w.norm[i + 4], 4) && strlen (w.norm[i + 4]) == 4)
            {
              year = atoi (w.norm[i + 4]);
              len = 5;
            }
          day = month_day (today, month_of (w.norm[i + 2]), atoi (t), year);
          use (&w, i, len);
          if (i > 0 && is (&w, i - 1, "el", NULL, NULL, NULL))
            use (&w, i - 1, 1);
        }
      else if (month_of (t) && strlen (t) >= 3 && i + 1 < w.n && !w.used[i + 1] && all_digits (w.norm[i + 1], 2))
        {
          day = month_day (today, month_of (t), atoi (w.norm[i + 1]), 0);
          use (&w, i, 2);
        }
      else if (all_digits (t, 2) && i + 1 < w.n && !w.used[i + 1] && month_of (w.norm[i + 1]) && strlen (w.norm[i + 1]) >= 3)
        {
          day = month_day (today, month_of (w.norm[i + 1]), atoi (t), 0);
          use (&w, i, 2);
        }
      else
        {
          int a, b, y = 0;
          /* 2026-10-20, the form the model and files use */
          int iy, im, id;
          if (strlen (t) == 10 && t[4] == '-' && t[7] == '-' && sscanf (t, "%4d-%2d-%2d", &iy, &im, &id) == 3 && im >= 1 && im <= 12 && id >= 1 && id <= 31)
            {
              day = at_midnight (iy, im, id);
              if (day)
                {
                  use (&w, i, 1);
                  continue;
                }
            }
          int cnt = sscanf (t, "%d/%d/%d", &a, &b, &y);
          if (cnt < 2)
            cnt = sscanf (t, "%d-%d-%d", &a, &b, &y);
          if ((cnt >= 2 && strchr (t, '/')) || (cnt == 3 && strchr (t, '-') && y > 31))
            {
              int dd = month_first ? b : a, mm = month_first ? a : b;
              if (mm > 12 && dd <= 12)
                {
                  int tmp = dd; dd = mm; mm = tmp;
                }
              if (mm >= 1 && mm <= 12 && dd >= 1 && dd <= 31)
                {
                  if (y && y < 100)
                    y += 2000;
                  day = month_day (today, mm, dd, cnt == 3 ? y : 0);
                  use (&w, i, 1);
                }
            }
          else if (i > 0 && is (&w, i - 1, "el", NULL, NULL, NULL) && all_digits (t, 2) && atoi (t) >= 1 && atoi (t) <= 31)
            {
              day = next_day_of_month (today, atoi (t));
              use (&w, i - 1, 2);
            }
        }
    }
  if (!day && repeat != CAL_REPEAT_NONE)
    day = today;
  if (!day && have_time)
    {
      /* "dentist 3pm": today, unless that time has passed */
      int h = clock_hour (&start_c, hint_start);
      day = at_clock (today, h, start_c.m) > now ? today : add_days (today, 1);
    }

  /* "dinner at 8" means 8 in the evening: the word in the title decides a bare hour */
  if (have_time && !start_c.sure && !start_c.ampm && !hint_start)
    {
      static const char *evening[] = { "cena", "cenar", "cenamos", "dinner", "supper", "noche", "night", "tonight", "evening", "nocturna" };
      for (guint i = 0; i < w.n && !hint_start; i++)
        for (guint k = 0; k < G_N_ELEMENTS (evening); k++)
          if (g_str_equal (w.norm[i], evening[k]) && start_c.h >= 5 && start_c.h <= 11)
            hint_start = 2;
    }

  /* assemble */
  gint64 start, end;
  gboolean all_day = !have_time;
  if (!day)
    day = today;
  if (all_day)
    {
      start = day;
      end = calendar_day_next (day);
    }
  else
    {
      int sh = clock_hour (&start_c, hint_start);
      start = at_clock (day, sh, start_c.m);
      if (have_end)
        {
          int eh = clock_hour (&end_c, hint_end ? hint_end : (end_c.ampm ? 0 : (start_c.ampm ? start_c.ampm : hint_start)));
          if (!end_c.ampm && !end_c.sure && end_c.h <= 12 && eh < sh && eh + 12 > sh)
            eh += 12;
          end = at_clock (day, eh, end_c.m);
          if (end <= start)
            end = add_days (end, 1);
        }
      else
        end = start + (duration_min ? duration_min : 60) * 60;
    }

  /* the title is what no rule used, minus connector words left at either end */
  static const char *edge[] = { "el", "la", "los", "las", "a", "de", "en", "at", "on", "the", "to", "from", "for",
                                "por", "este", "esta", "this", "del", "al", "-", "@" };
  guint lo = 0, hi = w.n;
  for (;;)
    {
      while (lo < hi && w.used[lo])
        lo++;
      while (hi > lo && w.used[hi - 1])
        hi--;
      if (lo >= hi)
        break;
      gboolean cut = FALSE;
      for (guint k = 0; k < G_N_ELEMENTS (edge); k++)
        {
          if (g_str_equal (w.norm[lo], edge[k]))
            {
              w.used[lo] = TRUE;
              cut = TRUE;
              break;
            }
          if (g_str_equal (w.norm[hi - 1], edge[k]))
            {
              w.used[hi - 1] = TRUE;
              cut = TRUE;
              break;
            }
        }
      if (!cut)
        break;
    }
  GString *title = g_string_new (NULL);
  for (guint i = 0; i < w.n; i++)
    if (!w.used[i])
      {
        if (title->len)
          g_string_append_c (title, ' ');
        g_string_append (title, w.raw[i]);
      }
  if (title->len)
    {
      /* capitalise the first letter, whatever alphabet it is in */
      gunichar first = g_utf8_get_char (title->str);
      g_autofree char *tail = g_strdup (g_utf8_next_char (title->str));
      char buf[8];
      int len = g_unichar_to_utf8 (g_unichar_toupper (first), buf);
      g_string_set_size (title, 0);
      g_string_append_len (title, buf, len);
      g_string_append (title, tail);
    }

  out->title = g_string_free (title, FALSE);
  out->start = start;
  out->end = end;
  out->all_day = all_day;
  out->repeat = repeat;
  words_free (&w);
  return TRUE;
}

CalEvent *
calendar_quick_to_event (const CalQuick *q, const char *untitled)
{
  CalEvent *ev = calendar_event_new (q->title && *q->title ? q->title : untitled, q->start, q->end, q->all_day);
  ev->repeat = q->repeat;
  ev->interval = q->repeat != CAL_REPEAT_NONE ? q->interval : 0;
  ev->until = q->repeat != CAL_REPEAT_NONE ? q->until : 0;
  ev->count = q->repeat != CAL_REPEAT_NONE ? q->count : 0;
  ev->reminder = q->reminder;
  if (q->reminder)
    ev->end = q->all_day ? calendar_day_next (q->start) : q->start + 900;
  if (q->alert >= 0)
    calendar_event_add_alert (ev, q->alert);
  return ev;
}
