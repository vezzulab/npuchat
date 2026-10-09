#include "skills.h"

#include <glib/gstdio.h>
#include <libsoup/soup.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>

#include "calendar-quick.h"
#include "calendar.h"
#include "i18n.h"
#include "net.h"
#include "sysinfo.h"

#define RESULT_LIMIT 1500 /* characters returned to the model: prefill is the cost */

static const Skill skills[] = {
  { "calculator", FALSE, "Calculadora", "Calculator",
    "Hace cuentas exactas en lugar de adivinarlas.", "Does exact math instead of guessing.", FALSE, TRUE },
  { "current_datetime", TRUE, "Fecha y hora", "Date and time",
    "Sabe qué día y qué hora es.", "Knows today's date and the time.", FALSE, TRUE },
  { "system_status", FALSE, "Estado del equipo", "Laptop status",
    "Consulta la batería, la memoria, el disco y la NPU.", "Checks battery, memory, disk and NPU.", FALSE, TRUE },
  { "calendar", FALSE, "Calendario", "Calendar",
    "Mira tu agenda y apunta eventos cuando se lo pides («apunta dentista mañana 3pm»). Lee y escribe solo en tu calendario local.",
    "Looks at your agenda and adds events when you ask (“add dentist tomorrow 3pm”). Reads and writes only your local calendar.",
    FALSE, TRUE },
  { "wikipedia", FALSE, "Wikipedia", "Wikipedia",
    "Busca datos en Wikipedia; pídelo con «busca en Wikipedia…». Envía el tema a wikipedia.org.",
    "Looks facts up on Wikipedia; ask with “look it up on Wikipedia…”. Sends the topic to wikipedia.org.",
    TRUE, FALSE },
};

const Skill *
skills_list (guint *count)
{
  *count = G_N_ELEMENTS (skills);
  return skills;
}

const Skill *
skill_find (const char *id)
{
  /* the calendar offers two tools, calendar_agenda and calendar_add */
  if (id && g_str_has_prefix (id, "calendar_"))
    id = "calendar";
  for (guint i = 0; id && i < G_N_ELEMENTS (skills); i++)
    if (g_str_equal (skills[i].id, id))
      return &skills[i];
  return NULL;
}

/* ---- tool definitions --------------------------------------------------- */

static void
add_function (JsonBuilder *b, const char *name, const char *description, const char *param, const char *param_description)
{
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "type");
  json_builder_add_string_value (b, "function");
  json_builder_set_member_name (b, "function");
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "name");
  json_builder_add_string_value (b, name);
  json_builder_set_member_name (b, "description");
  json_builder_add_string_value (b, description);
  json_builder_set_member_name (b, "parameters");
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "type");
  json_builder_add_string_value (b, "object");
  json_builder_set_member_name (b, "properties");
  json_builder_begin_object (b);
  if (param)
    {
      json_builder_set_member_name (b, param);
      json_builder_begin_object (b);
      json_builder_set_member_name (b, "type");
      json_builder_add_string_value (b, "string");
      json_builder_set_member_name (b, "description");
      json_builder_add_string_value (b, param_description);
      json_builder_end_object (b);
    }
  json_builder_end_object (b);
  if (param)
    {
      json_builder_set_member_name (b, "required");
      json_builder_begin_array (b);
      json_builder_add_string_value (b, param);
      json_builder_end_array (b);
    }
  json_builder_end_object (b);
  json_builder_end_object (b);
  json_builder_end_object (b);
}

void
skills_build_tools (JsonBuilder *b, gboolean (*enabled) (const char *id, gpointer data), gpointer data)
{
  json_builder_set_member_name (b, "tools");
  json_builder_begin_array (b);
  for (guint i = 0; i < G_N_ELEMENTS (skills); i++)
    {
      const char *id = skills[i].id;
      if (!enabled (id, data) || skills[i].context_only)
        continue;
      if (g_str_equal (id, "calculator"))
        add_function (b, id,
                      "Evaluate an arithmetic expression exactly and return the number. Always use it for any "
                      "calculation instead of computing in your head. Supports + - * / ^ ( ) and %, sqrt, abs, sin, "
                      "cos, tan, ln, log, exp, round, floor, ceil, min, max, pi and e. Use '.' for decimals.",
                      "expression", "The expression, for example 1847*392+15 or sqrt(2)*3");
      else if (g_str_equal (id, "system_status"))
        add_function (b, id,
                      "Get the status of the user's laptop: battery level and charging state, memory, free disk "
                      "space, load and the NPU. Use it when the user asks about their computer.", NULL, NULL);
      else if (g_str_equal (id, "calendar"))
        {
          add_function (b, "calendar_agenda",
                        "List the events in the user's calendar. Use it for any question about their schedule, "
                        "agenda, appointments or free time.",
                        "when", "today, tomorrow, week, next week, or a date like 2026-10-09");
          add_function (b, "calendar_add",
                        "Add an event to the user's calendar. Pass the event exactly as the user said it, with its "
                        "day and time, for example: dentist tomorrow 3pm. Do not work out dates yourself.",
                        "event", "The event in the user's own words, for example: dinner with Ana thursday 7pm");
        }
      else if (g_str_equal (id, "wikipedia"))
        add_function (b, id,
                      "Look up a topic on Wikipedia and return its summary. Use it for facts about people, places, "
                      "events and concepts you are not sure about. Pass only the topic name.",
                      "topic", "The topic, for example Ada Lovelace");
    }
  json_builder_end_array (b);
}

/* ---- calculator --------------------------------------------------------- */

typedef struct {
  const char *p;
  int         depth;
  const char *error; /* set once, never cleared */
} Parser;

static double parse_expr (Parser *ps);

static void
skip_space (Parser *ps)
{
  while (*ps->p == ' ' || *ps->p == '\t')
    ps->p++;
}

static double
fail (Parser *ps, const char *message)
{
  if (!ps->error)
    ps->error = message;
  return NAN;
}

static double
parse_number (Parser *ps)
{
  const char *start = ps->p;
  const char *q = start;
  while (g_ascii_isdigit (*q))
    q++;
  if (*q == '.')
    {
      q++;
      while (g_ascii_isdigit (*q))
        q++;
    }
  if (q == start || (q == start + 1 && *start == '.'))
    return fail (ps, "expected a number");
  /* An exponent only counts when digits follow ("2e3"); otherwise 'e' is the constant. */
  if ((*q == 'e' || *q == 'E') && (g_ascii_isdigit (q[1]) || ((q[1] == '+' || q[1] == '-') && g_ascii_isdigit (q[2]))))
    {
      q += 2;
      while (g_ascii_isdigit (*q))
        q++;
    }
  g_autofree char *text = g_strndup (start, q - start);
  ps->p = q;
  return g_ascii_strtod (text, NULL);
}

static gboolean
read_word (Parser *ps, char *out, size_t cap)
{
  size_t n = 0;
  while (g_ascii_isalpha (*ps->p) || (n > 0 && g_ascii_isdigit (*ps->p)))
    {
      if (n + 1 < cap)
        out[n++] = g_ascii_tolower (*ps->p);
      ps->p++;
    }
  out[n] = '\0';
  return n > 0;
}

static double
apply_function (Parser *ps, const char *name, double *a, int n)
{
  struct {
    const char *name;
    int         args;
    double    (*one) (double);
  } one_arg[] = {
    { "sqrt", 1, sqrt }, { "abs", 1, fabs }, { "sin", 1, sin }, { "cos", 1, cos }, { "tan", 1, tan },
    { "asin", 1, asin }, { "acos", 1, acos }, { "atan", 1, atan }, { "ln", 1, log }, { "log", 1, log10 },
    { "log2", 1, log2 }, { "exp", 1, exp }, { "floor", 1, floor }, { "ceil", 1, ceil }, { "round", 1, round },
  };
  for (guint i = 0; i < G_N_ELEMENTS (one_arg); i++)
    if (g_str_equal (name, one_arg[i].name))
      return n == 1 ? one_arg[i].one (a[0]) : fail (ps, "this function takes one argument");
  if (g_str_equal (name, "min") && n >= 2)
    return n == 2 ? fmin (a[0], a[1]) : fmin (fmin (a[0], a[1]), a[2]);
  if (g_str_equal (name, "max") && n >= 2)
    return n == 2 ? fmax (a[0], a[1]) : fmax (fmax (a[0], a[1]), a[2]);
  if (g_str_equal (name, "pow") && n == 2)
    return pow (a[0], a[1]);
  return fail (ps, "unknown function");
}

static double
parse_primary (Parser *ps)
{
  skip_space (ps);
  if (ps->error)
    return NAN;
  double v;

  if (*ps->p == '(')
    {
      ps->p++;
      v = parse_expr (ps);
      skip_space (ps);
      if (*ps->p != ')')
        return fail (ps, "missing closing parenthesis");
      ps->p++;
    }
  else if (g_ascii_isalpha (*ps->p))
    {
      char word[16];
      read_word (ps, word, sizeof word);
      skip_space (ps);
      if (*ps->p == '(')
        {
          ps->p++;
          double args[3];
          int n = 0;
          for (;;)
            {
              if (n == 3)
                return fail (ps, "too many arguments");
              args[n++] = parse_expr (ps);
              skip_space (ps);
              if (ps->error)
                return NAN;
              if (*ps->p != ',')
                break;
              ps->p++;
            }
          if (*ps->p != ')')
            return fail (ps, "missing closing parenthesis");
          ps->p++;
          v = apply_function (ps, word, args, n);
        }
      else if (g_str_equal (word, "pi"))
        v = G_PI;
      else if (g_str_equal (word, "e"))
        v = G_E;
      else if (g_str_equal (word, "tau"))
        v = 2 * G_PI;
      else
        return fail (ps, "unknown name");
    }
  else
    v = parse_number (ps);

  skip_space (ps);
  while (*ps->p == '%') /* "15%" means 0.15 */
    {
      v /= 100;
      ps->p++;
      skip_space (ps);
    }
  return v;
}

static double parse_unary (Parser *ps);

static double
parse_power (Parser *ps)
{
  double base = parse_primary (ps);
  skip_space (ps);
  if (ps->error)
    return NAN;
  if (*ps->p == '^' || (ps->p[0] == '*' && ps->p[1] == '*'))
    {
      ps->p += *ps->p == '^' ? 1 : 2;
      return pow (base, parse_unary (ps)); /* right-associative, allows 2^-1 */
    }
  return base;
}

static double
parse_unary (Parser *ps)
{
  skip_space (ps);
  if (++ps->depth > 64)
    return fail (ps, "expression is too deeply nested");
  double v;
  if (*ps->p == '-')
    {
      ps->p++;
      v = -parse_unary (ps); /* -2^2 is -(2^2) */
    }
  else if (*ps->p == '+')
    {
      ps->p++;
      v = parse_unary (ps);
    }
  else
    v = parse_power (ps);
  ps->depth--;
  return v;
}

static double
parse_term (Parser *ps)
{
  double v = parse_unary (ps);
  for (;;)
    {
      skip_space (ps);
      if (ps->error)
        return NAN;
      if (*ps->p == '*' && ps->p[1] != '*')
        {
          ps->p++;
          v *= parse_unary (ps);
        }
      else if (*ps->p == '/')
        {
          ps->p++;
          double d = parse_unary (ps);
          if (!ps->error && d == 0)
            return fail (ps, "division by zero");
          v /= d;
        }
      else
        return v;
    }
}

static double
parse_expr (Parser *ps)
{
  double v = parse_term (ps);
  for (;;)
    {
      skip_space (ps);
      if (ps->error)
        return NAN;
      if (*ps->p == '+')
        {
          ps->p++;
          v += parse_term (ps);
        }
      else if (*ps->p == '-')
        {
          ps->p++;
          v -= parse_term (ps);
        }
      else
        return v;
    }
}

static char *
format_number (double v)
{
  if (v == 0)
    return g_strdup ("0");
  if (fabs (v) < 1e15 && v == round (v))
    return g_strdup_printf ("%.0f", v);
  return g_strdup_printf ("%.12g", v);
}

gboolean
skill_calculate (const char *expression, char **result, char **error)
{
  *result = *error = NULL;
  if (!expression || !*expression)
    {
      *error = g_strdup ("Error: empty expression");
      return FALSE;
    }
  if (strlen (expression) > 200)
    {
      *error = g_strdup ("Error: expression is too long");
      return FALSE;
    }

  /* Typographic operators people (and models) paste in. */
  g_autoptr (GString) text = g_string_new (expression);
  g_string_replace (text, "×", "*", 0);
  g_string_replace (text, "·", "*", 0);
  g_string_replace (text, "÷", "/", 0);
  g_string_replace (text, "−", "-", 0);
  g_string_replace (text, "=", "", 0);

  Parser ps = { .p = text->str };
  double v = parse_expr (&ps);
  skip_space (&ps);
  if (!ps.error && *ps.p)
    ps.error = "unexpected character";
  if (!ps.error && !isfinite (v))
    ps.error = "the result is not a finite number";
  if (ps.error)
    {
      *error = g_strdup_printf ("Error: %s. Check the expression and try again.", ps.error);
      return FALSE;
    }
  *result = format_number (v);
  return TRUE;
}

/* ---- date and time ------------------------------------------------------ */

static const char *days_es[] = { "lunes", "martes", "miércoles", "jueves", "viernes", "sábado", "domingo" };
static const char *days_en[] = { "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday" };
static const char *months_es[] = { "enero", "febrero", "marzo", "abril", "mayo", "junio", "julio", "agosto",
                                   "septiembre", "octubre", "noviembre", "diciembre" };
static const char *months_en[] = { "January", "February", "March", "April", "May", "June", "July", "August",
                                   "September", "October", "November", "December" };

char *
skill_now_note (void)
{
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  int dow = g_date_time_get_day_of_week (now) - 1;
  int month = g_date_time_get_month (now) - 1;
  g_autofree char *tz = g_date_time_format (now, "%:z");
  g_autofree char *clock = g_date_time_format (now, "%H:%M");
  return i18n_lang () == LANG_EN
    ? g_strdup_printf ("[Context: it is now %s, %s %d, %d, %s (UTC%s).]", days_en[dow], months_en[month],
                       g_date_time_get_day_of_month (now), g_date_time_get_year (now), clock, tz)
    : g_strdup_printf ("[Context: it is now %s %d de %s de %d, %s (UTC%s).]", days_es[dow],
                       g_date_time_get_day_of_month (now), months_es[month], g_date_time_get_year (now), clock, tz);
}

/* Measured on qwen3.5:9b: without this the model answers sums from memory
 * and never calls Wikipedia, even when asked to; with it, no calls on 30
 * ordinary questions (jokes, greetings, translations). */
char *
skills_instructions (gboolean (*enabled) (const char *id, gpointer data), gpointer data)
{
  g_autoptr (GString) s = g_string_new (NULL);
  if (enabled ("calculator", data))
    g_string_append (s, " You MUST call calculator for every calculation, even trivial ones.");
  if (enabled ("system_status", data))
    g_string_append (s, " Call system_status for questions about this computer; battery and memory change, "
                        "so never reuse an earlier value.");
  if (enabled ("calendar", data))
    g_string_append (s, " You MUST call calendar_agenda for any question about the user's schedule, and calendar_add "
                        "when they ask you to schedule, add or remember an appointment or event. Pass their words as "
                        "they said them.");
  if (enabled ("wikipedia", data))
    g_string_append (s, " You MUST call wikipedia whenever the user asks about a specific person, place, organization "
                        "or event, or says 'search' or 'look up'. Never answer those from memory.");
  if (!s->len)
    return NULL;
  return g_strconcat ("TOOLS:", s->str, NULL);
}

char *
skill_datetime (void)
{
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  int dow = g_date_time_get_day_of_week (now) - 1;
  int month = g_date_time_get_month (now) - 1;
  g_autofree char *tz = g_date_time_format (now, "%:z");
  g_autofree char *clock = g_date_time_format (now, "%H:%M");
  g_autofree char *iso = g_date_time_format (now, "%Y-%m-%d");

  return i18n_lang () == LANG_EN
    ? g_strdup_printf ("%s, %s %d, %d, %s (UTC%s). ISO date: %s", days_en[dow], months_en[month],
                       g_date_time_get_day_of_month (now), g_date_time_get_year (now), clock, tz, iso)
    : g_strdup_printf ("%s %d de %s de %d, %s (UTC%s). Fecha ISO: %s", days_es[dow],
                       g_date_time_get_day_of_month (now), months_es[month], g_date_time_get_year (now), clock, tz, iso);
}

/* ---- calendar ----------------------------------------------------------- */

static char *
calendar_line (const CalOccurrence *o)
{
  g_autoptr (GDateTime) d = g_date_time_new_from_unix_local (o->start);
  int dow = g_date_time_get_day_of_week (d) - 1, month = g_date_time_get_month (d) - 1;
  g_autofree char *when = NULL;
  if (o->event->all_day)
    when = g_strdup (TR ("todo el día", "all day"));
  else
    {
      g_autoptr (GDateTime) e = g_date_time_new_from_unix_local (o->end);
      g_autofree char *a = g_date_time_format (d, "%H:%M");
      g_autofree char *b = g_date_time_format (e, "%H:%M");
      when = g_strdup_printf ("%s–%s", a, b);
    }
  const CalCalendar *c = calendar_calendar_find (calendar_default (), o->event->calendar);
  return g_strdup_printf ("- %s %d %s, %s: %s (%s)%s%s", i18n_lang () == LANG_EN ? days_en[dow] : days_es[dow],
                          g_date_time_get_day_of_month (d), i18n_lang () == LANG_EN ? months_en[month] : months_es[month],
                          when, o->event->title, c->name, *o->event->location ? " @ " : "", o->event->location);
}

char *
skill_calendar_agenda (const char *when)
{
  g_autofree char *trimmed = g_strstrip (g_strdup (when ? when : ""));
  g_autofree char *w = g_ascii_strdown (trimmed, -1);
  g_autofree char *ascii = g_str_to_ascii (w, NULL);
  gint64 today = calendar_day_start (g_get_real_time () / G_USEC_PER_SEC);
  gint64 from = today, to;
  gint64 t;
  gboolean date_only;
  if (!*ascii || g_strstr_len (ascii, -1, "today") || g_strstr_len (ascii, -1, "hoy"))
    to = calendar_day_next (today);
  else if (g_strstr_len (ascii, -1, "tomorrow") || g_strstr_len (ascii, -1, "manana"))
    {
      from = calendar_day_next (today);
      to = calendar_day_next (from);
    }
  else if (g_strstr_len (ascii, -1, "next week") || g_strstr_len (ascii, -1, "proxima semana"))
    {
      g_autoptr (GDateTime) d = g_date_time_new_from_unix_local (today);
      g_autoptr (GDateTime) a = g_date_time_add_days (d, 7);
      g_autoptr (GDateTime) b = g_date_time_add_days (d, 14);
      from = g_date_time_to_unix (a);
      to = g_date_time_to_unix (b);
    }
  else if (g_strstr_len (ascii, -1, "week") || g_strstr_len (ascii, -1, "semana"))
    {
      g_autoptr (GDateTime) d = g_date_time_new_from_unix_local (today);
      g_autoptr (GDateTime) b = g_date_time_add_days (d, 7);
      to = g_date_time_to_unix (b);
    }
  else if (calendar_parse_time (ascii, &t, &date_only))
    {
      from = calendar_day_start (t);
      to = calendar_day_next (from);
    }
  else
    return g_strdup ("Error: say today, tomorrow, week, next week, or a date like 2026-10-09.");

  GArray *occ = calendar_occurrences (calendar_default (), from, to);
  GString *out = g_string_new (NULL);
  guint n = 0;
  for (guint i = 0; i < occ->len && out->len < 1300; i++)
    {
      const CalOccurrence *o = &g_array_index (occ, CalOccurrence, i);
      if (!calendar_calendar_find (calendar_default (), o->event->calendar)->visible)
        continue;
      g_autofree char *line = calendar_line (o);
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

char *
skill_calendar_add (const char *text, gboolean *ok)
{
  CalQuick q;
  *ok = FALSE;
  if (!text || !calendar_quick_parse (text, g_get_real_time () / G_USEC_PER_SEC, i18n_lang () == LANG_EN, &q))
    return g_strdup ("Error: describe the event, for example: dentist tomorrow 3pm.");
  if (!*q.title)
    {
      calendar_quick_clear (&q);
      return g_strdup ("Error: the event needs a name, for example: dentist tomorrow 3pm.");
    }
  CalEvent *ev = calendar_event_new (q.title, q.start, q.end, q.all_day);
  ev->repeat = q.repeat;
  CalOccurrence o = { ev, q.start, q.end };
  g_autofree char *line = calendar_line (&o);
  calendar_add (calendar_default (), ev);
  calendar_quick_clear (&q);
  *ok = TRUE;
  return g_strdup_printf ("%s %s", TR ("Apuntado:", "Added:"), line + 2);
}

/* ---- laptop status ------------------------------------------------------ */

static char *
read_line (const char *path)
{
  char *contents = NULL;
  if (!g_file_get_contents (path, &contents, NULL, NULL))
    return NULL;
  return g_strstrip (contents);
}

static double
read_number (const char *dir, const char *name)
{
  g_autofree char *path = g_build_filename (dir, name, NULL);
  g_autofree char *line = read_line (path);
  return line ? g_ascii_strtod (line, NULL) : -1;
}

char *
skill_system_status (void)
{
  g_autoptr (GString) out = g_string_new (NULL);

  /* Peripherals (a stylus, a touchscreen, a mouse) also show up as "Battery"
   * with scope Device; only the laptop's own battery counts. */
  g_autoptr (GDir) dir = g_dir_open ("/sys/class/power_supply", 0, NULL);
  g_autoptr (GPtrArray) names = g_ptr_array_new_with_free_func (g_free);
  const char *name;
  while (dir && (name = g_dir_read_name (dir)))
    g_ptr_array_add (names, g_strdup (name));
  g_ptr_array_sort_values (names, (GCompareFunc) g_strcmp0);

  guint batteries = 0;
  gboolean plugged = FALSE;
  for (guint i = 0; i < names->len; i++)
    {
      g_autofree char *base = g_build_filename ("/sys/class/power_supply", names->pdata[i], NULL);
      g_autofree char *type_path = g_build_filename (base, "type", NULL);
      g_autofree char *type = read_line (type_path);
      g_autofree char *scope_path = g_build_filename (base, "scope", NULL);
      g_autofree char *scope = read_line (scope_path);
      if (g_strcmp0 (type, "Battery") == 0)
        {
          if (g_strcmp0 (scope, "Device") == 0 || read_number (base, "present") == 0)
            continue;
          batteries++;
          g_autofree char *state_path = g_build_filename (base, "status", NULL);
          g_autofree char *state = read_line (state_path);
          double level = read_number (base, "capacity");
          g_string_append_printf (out, "Battery: %.0f%%, %s", level, state ? state : "unknown");
          double energy = read_number (base, "energy_now"), power = read_number (base, "power_now");
          if (g_strcmp0 (state, "Discharging") == 0 && energy > 0 && power > 0)
            g_string_append_printf (out, ", about %.1f hours left at the current draw", energy / power);
          g_string_append_c (out, '\n');
        }
      else if (g_strcmp0 (type, "Mains") == 0 && read_number (base, "online") > 0)
        plugged = TRUE;
    }
  if (batteries)
    g_string_append_printf (out, "Power: %s\n", plugged ? "plugged in" : "running on battery");
  else
    g_string_append (out, "Battery: none (a desktop computer)\n");

  g_autoptr (SysInfo) info = sysinfo_get ();
  if (info->ram_total_gb > 0)
    {
      g_autofree char *meminfo = NULL;
      double available = 0;
      if (g_file_get_contents ("/proc/meminfo", &meminfo, NULL, NULL))
        {
          const char *line = strstr (meminfo, "MemAvailable:");
          if (line)
            available = g_ascii_strtod (line + strlen ("MemAvailable:"), NULL) / (1024.0 * 1024.0);
        }
      g_string_append_printf (out, "Memory: %.1f GB total, %.1f GB available\n", info->ram_total_gb, available);
    }
  g_string_append_printf (out, "Disk: %.0f GB free in the home folder\n", info->disk_free_gb);
  if (info->cpu)
    g_string_append_printf (out, "Processor: %s\n", info->cpu);
  g_autofree char *load = read_line ("/proc/loadavg");
  if (load)
    {
      char *space = strchr (load, ' ');
      if (space)
        *space = '\0';
      g_string_append_printf (out, "CPU load (1 min): %s\n", load);
    }
  if (info->npu_present)
    g_string_append_printf (out, "NPU: %s%s%s\n", info->npu_name, info->npu_firmware ? ", firmware " : "",
                            info->npu_firmware ? info->npu_firmware : "");
  g_autofree char *kernel = info->kernel ? g_strdup (info->kernel) : NULL;
  if (kernel)
    g_string_append_printf (out, "Kernel: %s\n", kernel);
  return g_string_free (g_steal_pointer (&out), FALSE);
}

/* ---- Wikipedia ---------------------------------------------------------- */

typedef struct {
  SkillDone     done;
  gpointer      data;
  GCancellable *cancel;
  char         *topic;
  char         *base; /* https://es.wikipedia.org, or a test server */
  gboolean      searched;
} WikiCtx;

static void wiki_fetch_summary (WikiCtx *ctx, const char *title);

static void
wiki_finish (WikiCtx *ctx, char *result, gboolean ok)
{
  ctx->done (result, ok, ctx->data);
  g_clear_object (&ctx->cancel);
  g_free (ctx->topic);
  g_free (ctx->base);
  g_free (ctx);
}

/* Cuts at the end of a sentence near the limit so the model gets whole ideas. */
static char *
trim_extract (const char *text)
{
  if (strlen (text) <= RESULT_LIMIT)
    return g_strdup (text);
  g_autofree char *cut = g_strndup (text, RESULT_LIMIT);
  char *dot = strrchr (cut, '.');
  if (dot && dot > cut + RESULT_LIMIT / 2)
    dot[1] = '\0';
  return g_steal_pointer (&cut);
}

static void
wiki_search_done (GObject *src, GAsyncResult *res, gpointer user_data)
{
  WikiCtx *ctx = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) body = soup_session_send_and_read_finish (SOUP_SESSION (src), res, &error);
  g_autoptr (JsonParser) parser = json_parser_new ();

  if (!body || !json_parser_load_from_data (parser, g_bytes_get_data (body, NULL), g_bytes_get_size (body), NULL))
    {
      wiki_finish (ctx, g_strdup_printf ("Error: could not reach Wikipedia (%s).", error ? error->message : "bad reply"), FALSE);
      return;
    }
  JsonNode *root = json_parser_get_root (parser);
  JsonObject *o = JSON_NODE_HOLDS_OBJECT (root) ? json_node_get_object (root) : NULL;
  JsonObject *query = o && json_object_has_member (o, "query") ? json_object_get_object_member (o, "query") : NULL;
  JsonArray *hits = query && json_object_has_member (query, "search") ? json_object_get_array_member (query, "search") : NULL;
  JsonObject *first = hits && json_array_get_length (hits) > 0 ? json_array_get_object_element (hits, 0) : NULL;
  const char *title = first ? json_object_get_string_member_with_default (first, "title", NULL) : NULL;

  if (!title)
    {
      wiki_finish (ctx, g_strdup_printf ("No Wikipedia article was found for \"%s\".", ctx->topic), FALSE);
      return;
    }
  wiki_fetch_summary (ctx, title);
}

static void
wiki_summary_done (GObject *src, GAsyncResult *res, gpointer user_data)
{
  WikiCtx *ctx = user_data;
  g_autoptr (GError) error = NULL;
  SoupMessage *msg = soup_session_get_async_result_message (SOUP_SESSION (src), res);
  g_autoptr (GBytes) body = soup_session_send_and_read_finish (SOUP_SESSION (src), res, &error);

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      wiki_finish (ctx, g_strdup ("Cancelled."), FALSE);
      return;
    }
  if (!body)
    {
      wiki_finish (ctx, g_strdup_printf ("Error: could not reach Wikipedia (%s).", error->message), FALSE);
      return;
    }

  if (soup_message_get_status (msg) == SOUP_STATUS_NOT_FOUND && !ctx->searched)
    {
      /* The exact title does not exist: let Wikipedia's own search pick the closest. */
      ctx->searched = TRUE;
      g_autofree char *q = g_uri_escape_string (ctx->topic, NULL, FALSE);
      g_autofree char *url = g_strdup_printf ("%s/w/api.php?action=query&list=search&srsearch=%s&format=json&srlimit=1&utf8=1",
                                              ctx->base, q);
      g_autoptr (SoupMessage) next = soup_message_new ("GET", url);
      soup_session_send_and_read_async (net_session (), next, G_PRIORITY_DEFAULT, ctx->cancel, wiki_search_done, ctx);
      return;
    }
  if (soup_message_get_status (msg) != SOUP_STATUS_OK)
    {
      wiki_finish (ctx, g_strdup_printf ("No Wikipedia article was found for \"%s\".", ctx->topic), FALSE);
      return;
    }

  g_autoptr (JsonParser) parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, g_bytes_get_data (body, NULL), g_bytes_get_size (body), NULL) ||
      !JSON_NODE_HOLDS_OBJECT (json_parser_get_root (parser)))
    {
      wiki_finish (ctx, g_strdup ("Error: Wikipedia sent an unreadable reply."), FALSE);
      return;
    }
  JsonObject *o = json_node_get_object (json_parser_get_root (parser));
  const char *title = json_object_get_string_member_with_default (o, "title", ctx->topic);
  const char *blurb = json_object_get_string_member_with_default (o, "description", "");
  const char *extract = json_object_get_string_member_with_default (o, "extract", "");
  const char *type = json_object_get_string_member_with_default (o, "type", "");
  g_autofree char *summary = trim_extract (extract);

  const char *page = NULL;
  JsonObject *urls = json_object_has_member (o, "content_urls") ? json_object_get_object_member (o, "content_urls") : NULL;
  JsonObject *desktop = urls && json_object_has_member (urls, "desktop") ? json_object_get_object_member (urls, "desktop") : NULL;
  if (desktop)
    page = json_object_get_string_member_with_default (desktop, "page", NULL);

  g_autoptr (GString) out = g_string_new (NULL);
  g_string_append_printf (out, "Wikipedia: %s", title);
  if (*blurb)
    g_string_append_printf (out, " (%s)", blurb);
  g_string_append_printf (out, "\n%s", *summary ? summary : "(no summary available)");
  if (g_str_equal (type, "disambiguation"))
    g_string_append (out, "\nNote: this is a disambiguation page; ask the user which meaning they want.");
  if (page)
    g_string_append_printf (out, "\nSource: %s", page);
  wiki_finish (ctx, g_string_free (g_steal_pointer (&out), FALSE), TRUE);
}

static void
wiki_fetch_summary (WikiCtx *ctx, const char *title)
{
  g_autofree char *escaped = g_uri_escape_string (title, NULL, FALSE);
  g_autofree char *url = g_strdup_printf ("%s/api/rest_v1/page/summary/%s", ctx->base, escaped);
  g_autoptr (SoupMessage) msg = soup_message_new ("GET", url);
  soup_session_send_and_read_async (net_session (), msg, G_PRIORITY_DEFAULT, ctx->cancel, wiki_summary_done, ctx);
}

static void
run_wikipedia (const char *topic, GCancellable *cancel, SkillDone done, gpointer data)
{
  if (!topic || !*topic)
    {
      done (g_strdup ("Error: the topic is empty."), FALSE, data);
      return;
    }
  WikiCtx *ctx = g_new0 (WikiCtx, 1);
  ctx->done = done;
  ctx->data = data;
  ctx->cancel = cancel ? g_object_ref (cancel) : NULL;
  ctx->topic = g_strdup (topic);
  /* A test server stands in for wikipedia.org. */
  const char *override = g_getenv ("NPU_CHAT_WIKI_URL");
  ctx->base = override && *override ? g_strdup (override)
                                    : g_strdup_printf ("https://%s.wikipedia.org", i18n_lang () == LANG_EN ? "en" : "es");
  wiki_fetch_summary (ctx, topic);
}

/* ---- dispatch ----------------------------------------------------------- */

void
skill_run (const char *id, const char *arguments_json, GCancellable *cancel, SkillDone done, gpointer data)
{
  g_autoptr (JsonParser) parser = json_parser_new ();
  JsonObject *args = NULL;
  if (arguments_json && *arguments_json && json_parser_load_from_data (parser, arguments_json, -1, NULL) &&
      JSON_NODE_HOLDS_OBJECT (json_parser_get_root (parser)))
    args = json_node_get_object (json_parser_get_root (parser));

  if (g_str_equal (id, "calculator"))
    {
      const char *expr = args ? json_object_get_string_member_with_default (args, "expression", NULL) : NULL;
      g_autofree char *result = NULL;
      g_autofree char *error = NULL;
      gboolean ok = skill_calculate (expr, &result, &error);
      done (ok ? g_steal_pointer (&result) : g_steal_pointer (&error), ok, data);
    }
  else if (g_str_equal (id, "current_datetime"))
    done (skill_datetime (), TRUE, data);
  else if (g_str_equal (id, "system_status"))
    done (skill_system_status (), TRUE, data);
  else if (g_str_equal (id, "calendar_agenda"))
    {
      char *r = skill_calendar_agenda (args ? json_object_get_string_member_with_default (args, "when", "today") : "today");
      done (r, !g_str_has_prefix (r, "Error"), data);
    }
  else if (g_str_equal (id, "calendar_add"))
    {
      gboolean added;
      char *r = skill_calendar_add (args ? json_object_get_string_member_with_default (args, "event", NULL) : NULL, &added);
      done (r, added, data);
    }
  else if (g_str_equal (id, "wikipedia"))
    run_wikipedia (args ? json_object_get_string_member_with_default (args, "topic", NULL) : NULL, cancel, done, data);
  else
    done (g_strdup_printf ("Error: there is no tool called \"%s\".", id), FALSE, data);
}
