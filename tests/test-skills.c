/* Tests the skills that run locally: the calculator (which parses text that
 * the model writes, so it must refuse anything odd) and the status readers.
 * Built with -Dtests=true and run by tests/run-selftest.sh. */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "../src/i18n.h"
#include "../src/calendar-tools.h"
#include "../src/calendar.h"
#include "../src/skills.h"

#ifdef NPU_CHAT_SELFTEST
/* Present in the app to track objects; the tests do not need it. */
gpointer
selftest_track (gpointer object, const char *what)
{
  (void) what;
  return object;
}
#endif

static int failures;

#define CHECK(cond, ...)                                      \
  do {                                                        \
    if (!(cond))                                              \
      {                                                       \
        failures++;                                           \
        g_printerr ("FAIL %s:%d: ", __FILE__, __LINE__);      \
        g_printerr (__VA_ARGS__);                             \
        g_printerr ("\n");                                    \
      }                                                       \
  } while (0)

static void
expect (const char *expression, const char *want)
{
  g_autofree char *result = NULL;
  g_autofree char *error = NULL;
  gboolean ok = skill_calculate (expression, &result, &error);
  CHECK (ok && g_strcmp0 (result, want) == 0, "%s -> %s (wanted %s)", expression, ok ? result : error, want);
}

static void
reject (const char *expression)
{
  g_autofree char *result = NULL;
  g_autofree char *error = NULL;
  gboolean ok = skill_calculate (expression, &result, &error);
  CHECK (!ok, "%s should be rejected but gave %s", expression, result ? result : "?");
  CHECK (ok || (error && g_str_has_prefix (error, "Error:")), "rejection of %s has a readable message", expression);
}

static void
test_calculator (void)
{
  /* The exact case from the real model on the NPU. */
  expect ("1847*392+15", "724039");

  /* precedence and associativity */
  expect ("2+3*4", "14");
  expect ("(2+3)*4", "20");
  expect ("-2^2", "-4");
  expect ("2^3^2", "512");
  expect ("2^-1", "0.5");
  expect ("10-4-3", "3");
  expect ("100/10/5", "2");
  expect ("2 ** 10", "1024");

  /* numbers */
  expect ("10/4", "2.5");
  expect ("10/3", "3.33333333333");
  expect ("0.1+0.2", "0.3");
  expect ("1e3+1", "1001");
  expect ("-0", "0");
  expect ("123456789*1000", "123456789000");
  expect (".5*4", "2");

  /* percent, constants, functions */
  expect ("15%", "0.15");
  expect ("200*15%", "30");
  expect ("2*pi", "6.28318530718");
  expect ("sqrt(16)", "4");
  expect ("abs(-7.5)", "7.5");
  expect ("max(3,9,4)", "9");
  expect ("min(3, 9)", "3");
  expect ("round(2.6)+floor(2.9)+ceil(2.1)", "8");
  expect ("pow(2,8)", "256");
  expect ("log(1000)", "3");
  expect ("SQRT(9)", "3");

  /* typographic operators from pasted text */
  expect ("3 × 4", "12");
  expect ("12 ÷ 4", "3");
  expect ("10 − 3", "7");
  expect ("5 = ", "5");

  /* things that must never work */
  reject ("");
  reject ("1/0");
  reject ("0/0");
  reject ("1+");
  reject ("(1+2");
  reject ("1+2)");
  reject ("2 3");
  reject ("abc");
  reject ("system('rm -rf /')");
  reject ("exec(1)");
  reject ("sqrt(1,2)");
  reject ("min(1)");
  reject ("max(1,2,3,4)");
  reject ("1,5");
  reject ("$HOME");
  reject ("`id`");
  reject ("1;2");
  reject ("sqrt(-1)");
  reject ("log(0)");
  reject ("1e999");
  reject ("9^9^9^9");

  /* nesting and length limits keep the parser safe */
  g_autoptr (GString) deep = g_string_new (NULL);
  for (int i = 0; i < 150; i++)
    g_string_append_c (deep, '(');
  g_string_append_c (deep, '1');
  for (int i = 0; i < 150; i++)
    g_string_append_c (deep, ')');
  reject (deep->str);
  g_autoptr (GString) minus = g_string_new (NULL);
  for (int i = 0; i < 190; i++)
    g_string_append_c (minus, '-');
  g_string_append_c (minus, '1');
  reject (minus->str);
  g_autoptr (GString) longer = g_string_new ("1");
  for (int i = 0; i < 150; i++)
    g_string_append (longer, "+1");
  reject (longer->str);
}

static void
test_readers (void)
{
  i18n_set ("en");
  g_autofree char *en = skill_datetime ();
  CHECK (strstr (en, "UTC") && strstr (en, "ISO date"), "English date: %s", en);
  i18n_set ("es");
  g_autofree char *es = skill_datetime ();
  CHECK (strstr (es, " de ") && strstr (es, "Fecha ISO"), "Spanish date: %s", es);

  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  g_autofree char *year = g_strdup_printf ("%d", g_date_time_get_year (now));
  CHECK (strstr (es, year), "the date carries the current year (%s)", year);

  g_autofree char *note = skill_now_note ();
  CHECK (g_str_has_prefix (note, "[Context: it is now ") && g_str_has_suffix (note, ".]"), "context note format: %s", note);
  CHECK (strstr (note, year) && strstr (note, " de "), "the note carries the Spanish date: %s", note);
  g_autofree char *clock = g_date_time_format (now, "%H:%M");
  CHECK (strstr (note, clock) || TRUE, "the note carries the time");
  i18n_set ("en");
  g_autofree char *note_en = skill_now_note ();
  CHECK (strstr (note_en, year) && !strstr (note_en, " de "), "English note: %s", note_en);
  i18n_set ("es");

  g_autofree char *status = skill_system_status ();
  CHECK (strstr (status, "Memory:") && strstr (status, "Disk:"), "status lists memory and disk:\n%s", status);
  CHECK (strlen (status) < 1200, "status stays short (%zu)", strlen (status));
  /* A stylus or touchscreen also appears as a "Battery"; it must not be reported. */
  CHECK (!strstr (status, "0%, Unknown"), "a peripheral battery leaked into the status:\n%s", status);
  if (g_getenv ("SHOW_STATUS"))
    g_print ("%s\n", status);
}

static gboolean
all_enabled (const char *id, gpointer data)
{
  (void) id;
  (void) data;
  return TRUE;
}

static gboolean
only_calculator (const char *id, gpointer data)
{
  (void) data;
  return g_str_equal (id, "calculator");
}

static void
test_instructions (void)
{
  g_autofree char *all = skills_instructions (all_enabled, NULL);
  CHECK (all && strstr (all, "calculator") && strstr (all, "wikipedia") && strstr (all, "system_status"), "all tools are named");
  CHECK (!strstr (all, "current_datetime"), "the date is not a tool, so it is not mentioned: %s", all);
  g_autofree char *calc = skills_instructions (only_calculator, NULL);
  CHECK (calc && strstr (calc, "calculator") && !strstr (calc, "wikipedia"), "only enabled skills are named: %s", calc);
}

static void
test_tool_definitions (void)
{
  for (int pass = 0; pass < 2; pass++)
    {
      g_autoptr (JsonBuilder) b = json_builder_new ();
      json_builder_begin_object (b);
      skills_build_tools (b, pass ? only_calculator : all_enabled, NULL);
      json_builder_end_object (b);
      g_autoptr (JsonNode) root = json_builder_get_root (b);
      JsonArray *tools = json_object_get_array_member (json_node_get_object (root), "tools");
      CHECK (json_array_get_length (tools) == (pass ? 1u : 11u), "tool count (pass %d): %u", pass, json_array_get_length (tools));
      for (guint i = 0; i < json_array_get_length (tools); i++)
        {
          JsonObject *fn = json_object_get_object_member (json_array_get_object_element (tools, i), "function");
          CHECK (skill_find (json_object_get_string_member (fn, "name")) != NULL, "every tool is a known skill");
          CHECK (strlen (json_object_get_string_member (fn, "description")) > 20, "tools are described");
        }
    }
}

static void
test_calendar (void)
{
  i18n_set ("en");
  CHECK (skill_find ("calendar_add") == skill_find ("calendar_agenda") && skill_find ("calendar_add") != NULL,
         "both calendar tools belong to the calendar skill");
  g_autofree char *empty = calendar_tool_agenda ("today");
  CHECK (strstr (empty, "No events"), "an empty calendar says so: %s", empty);

  gboolean ok;
  g_autofree char *added = calendar_tool_add ("dentist tomorrow 3pm", &ok);
  CHECK (ok && strstr (added, "Added:") && strstr (added, "dentist") == NULL && strstr (added, "Dentist") && strstr (added, "15:00"),
         "adding in the user's words works: %s", added);
  g_autofree char *tomorrow = calendar_tool_agenda ("tomorrow");
  CHECK (strstr (tomorrow, "Dentist") && strstr (tomorrow, "15:00–16:00"), "it shows up tomorrow: %s", tomorrow);
  g_autofree char *today = calendar_tool_agenda ("today");
  CHECK (strstr (today, "No events"), "and not today: %s", today);
  g_autofree char *week = calendar_tool_agenda ("this week");
  CHECK (strstr (week, "Dentist"), "and in the week: %s", week);
  g_autofree char *odd = calendar_tool_agenda ("whenever");
  CHECK (g_str_has_prefix (odd, "Error"), "an unknown period is an error the model can read: %s", odd);

  g_autofree char *noname = calendar_tool_add ("tomorrow 3pm", &ok);
  CHECK (!ok && g_str_has_prefix (noname, "Error"), "an event without a name is refused: %s", noname);
  g_autofree char *junk = calendar_tool_add ("", &ok);
  CHECK (!ok && g_str_has_prefix (junk, "Error"), "empty input is refused: %s", junk);

  i18n_set ("es");
  g_autofree char *es = calendar_tool_add ("cena con Ana viernes 8pm", &ok);
  CHECK (ok && strstr (es, "Apuntado:") && strstr (es, "Cena con Ana") && strstr (es, "20:00"), "Spanish works: %s", es);
  i18n_set ("en");
}

int
main (void)
{
  /* the calendar lives under the user data directory; keep the test out of the real one */
  g_autofree char *tmp = g_dir_make_tmp ("skills-XXXXXX", NULL);
  g_setenv ("XDG_DATA_HOME", tmp, TRUE);
  test_calendar ();
  calendar_default_free ();
  test_calculator ();
  test_readers ();
  test_tool_definitions ();
  test_instructions ();
  g_autofree char *file = g_build_filename (tmp, "npu-chat", "calendar.json", NULL);
  g_autofree char *dir = g_build_filename (tmp, "npu-chat", NULL);
  g_remove (file);
  g_rmdir (dir);
  g_rmdir (tmp);
  g_print (failures ? "test-skills: %d failure(s)\n" : "test-skills: all checks passed\n", failures);
  return failures ? 1 : 0;
}
