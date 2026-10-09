#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "../src/calendar-tools.h"
#include "../src/calendar.h"
#include "../src/i18n.h"

static int failures;

#define CHECK(cond, ...) \
  G_STMT_START { \
    if (!(cond)) { \
      failures++; \
      g_printerr ("FAIL line %d: ", __LINE__); \
      g_printerr (__VA_ARGS__); \
      g_printerr ("\n"); \
    } \
  } G_STMT_END

static char *
iso_in (int days, const char *clock)
{
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  g_autoptr (GDateTime) d = g_date_time_add_days (now, days);
  g_autofree char *date = g_date_time_format (d, "%Y-%m-%d");
  return g_strdup_printf ("%s %s", date, clock);
}

int
main (void)
{
  g_autofree char *tmp = g_dir_make_tmp ("tools-XXXXXX", NULL);
  g_setenv ("XDG_DATA_HOME", tmp, TRUE);
  g_setenv ("XDG_CONFIG_HOME", tmp, TRUE);
  i18n_set ("en");
  gboolean ok;

  /* add, then see it */
  g_autofree char *when = iso_in (3, "15:00");
  g_autofree char *add_text = g_strdup_printf ("dentist %s", when);
  g_autofree char *added = calendar_tool_add (add_text, &ok);
  CHECK (ok && strstr (added, "Added:") && strstr (added, "Dentist") && strstr (added, "15:00"), "add: %s", added);
  g_autofree char *day3 = iso_in (3, "");
  g_strstrip (day3);
  g_autofree char *agenda = calendar_tool_agenda (day3);
  CHECK (strstr (agenda, "Dentist"), "agenda shows it: %s", agenda);

  /* a reminder with an alert, and a repeating event */
  g_autofree char *rent_when = iso_in (4, "9:00");
  g_autofree char *rem_text = g_strdup_printf ("remind me to pay rent %s", rent_when);
  g_autofree char *rem = calendar_tool_add (rem_text, &ok);
  CHECK (ok && strstr (rem, "Pay rent") && strstr (rem, "[reminder]"), "reminder: %s", rem);
  g_autofree char *rep = calendar_tool_add ("gym every day 6am tomorrow 30 min before", &ok);
  CHECK (ok && strstr (rep, "Gym"), "repeating: %s", rep);

  /* find */
  g_autofree char *found = calendar_tool_find ("dent");
  CHECK (strstr (found, "Dentist"), "find: %s", found);
  g_autofree char *none = calendar_tool_find ("zzzz");
  CHECK (strstr (none, "No events"), "find nothing: %s", none);

  /* change by title: new day and time */
  g_autofree char *new_time = iso_in (5, "17:30");
  g_autofree char *changed = calendar_tool_change ("dentist", new_time, NULL, "Clinic 4", &ok);
  CHECK (ok && strstr (changed, "Changed:") && strstr (changed, "17:30") && strstr (changed, "Clinic 4"), "change: %s", changed);
  g_autofree char *moved = calendar_tool_agenda (day3);
  CHECK (!strstr (moved, "Dentist"), "it left the old day: %s", moved);
  g_autofree char *day5 = iso_in (5, "");
  g_strstrip (day5);
  g_autofree char *there = calendar_tool_agenda (day5);
  CHECK (strstr (there, "Dentist") && strstr (there, "17:30"), "and is on the new one: %s", there);

  /* only a day given: the clock time stays */
  g_autofree char *day6 = iso_in (6, "");
  g_strstrip (day6);
  g_autofree char *keep = calendar_tool_change ("dentist", day6, NULL, NULL, &ok);
  CHECK (ok && strstr (keep, "17:30"), "the time is kept: %s", keep);

  /* several matches are never guessed */
  g_autofree char *second_when = iso_in (7, "9:00");
  g_autofree char *second = g_strdup_printf ("dentist cleaning %s", second_when);
  g_autofree char *added2 = calendar_tool_add (second, &ok);
  CHECK (ok, "second dentist: %s", added2);
  g_autofree char *amb = calendar_tool_change ("dentist", "tomorrow", NULL, NULL, &ok);
  CHECK (!ok && strstr (amb, "several"), "ambiguous: %s", amb);
  g_autofree char *amb_del = calendar_tool_delete ("dent", &ok);
  CHECK (!ok && strstr (amb_del, "several"), "ambiguous delete: %s", amb_del);

  /* delete one, bring it back */
  g_autofree char *del = calendar_tool_delete ("cleaning", &ok);
  CHECK (ok && strstr (del, "Deleted:") && strstr (del, "undo"), "delete: %s", del);
  g_autofree char *gone = calendar_tool_find ("cleaning");
  CHECK (strstr (gone, "No events"), "it is gone: %s", gone);
  g_autofree char *undo = calendar_tool_undo (&ok);
  CHECK (ok && strstr (undo, "Restored:") && strstr (undo, "Dentist cleaning"), "undo: %s", undo);
  g_autofree char *back = calendar_tool_find ("cleaning");
  CHECK (strstr (back, "Dentist cleaning"), "it is back: %s", back);
  g_autofree char *undo2 = calendar_tool_undo (&ok);
  CHECK (!ok && strstr (undo2, "nothing to undo"), "nothing more: %s", undo2);
  g_autofree char *del_none = calendar_tool_delete ("unicorn", &ok);
  CHECK (!ok && strstr (del_none, "no event matches"), "delete nothing: %s", del_none);

  /* a repeating event: only that showing moves */
  g_autofree char *gym_new = iso_in (1, "7:15");
  g_autofree char *gym = calendar_tool_change ("gym", gym_new, NULL, NULL, &ok);
  CHECK (ok && strstr (gym, "Changed:"), "change one showing: %s", gym);
  g_autofree char *day1 = iso_in (1, "");
  g_strstrip (day1);
  g_autofree char *ag1 = calendar_tool_agenda (day1);
  CHECK (strstr (ag1, "07:15") && !strstr (ag1, "06:00"), "the next showing moved: %s", ag1);
  g_autofree char *day2 = iso_in (2, "");
  g_strstrip (day2);
  g_autofree char *ag2 = calendar_tool_agenda (day2);
  CHECK (strstr (ag2, "06:00"), "the following ones did not: %s", ag2);

  /* reminders */
  g_autofree char *done = calendar_tool_done ("pay rent", &ok);
  CHECK (ok && strstr (done, "Marked done"), "done: %s", done);
  g_autofree char *day4 = iso_in (4, "");
  g_strstrip (day4);
  g_autofree char *ag4 = calendar_tool_agenda (day4);
  CHECK (strstr (ag4, "[done]"), "it shows as done: %s", ag4);
  g_autofree char *not_rem = calendar_tool_done ("cleaning", &ok);
  CHECK (!ok && strstr (not_rem, "not a reminder"), "events are not reminders: %s", not_rem);

  /* free time: tomorrow has the gym at 06:00 only, so 14:00-15:00 style gaps exist */
  g_autofree char *free_slots = calendar_tool_free ("60", "tomorrow");
  CHECK (strstr (free_slots, "min)"), "free: %s", free_slots);
  g_autofree char *no_free = calendar_tool_free ("700", "tomorrow");
  CHECK (strstr (no_free, "no gap"), "no 15-hour gap: %s", no_free);

  /* a time copied from the context note is not the user's */
  g_autofree char *leak_when = iso_in (9, "");
  g_strstrip (leak_when);
  g_autofree char *leak_text = g_strdup_printf ("pay the water bill %s 20:10 (UTC-04:00)", leak_when);
  g_autofree char *leaked = calendar_tool_add (leak_text, &ok);
  CHECK (ok && strstr (leaked, "Pay the water bill") && !strstr (leaked, "20:10") && strstr (leaked, "all day"), "context time removed: %s", leaked);
  g_autofree char *leak2 = calendar_tool_add ("remind me to call mom friday 20:10 (UTC-04:00)", &ok);
  CHECK (ok && strstr (leak2, "Call mom") && !strstr (leak2, "20:10") && !strstr (leak2, "UTC"), "and for reminders: %s", leak2);

  /* every function survives nonsense */
  g_autofree char *big = g_strnfill (5000, 'x');
  const char *junk[] = { NULL, "", "   ", "\n\n", big, "😀", "'; DROP TABLE", "-1", "99999999999999999999" };
  for (guint i = 0; i < G_N_ELEMENTS (junk); i++)
    {
      g_free (calendar_tool_agenda (junk[i]));
      g_free (calendar_tool_add (junk[i], &ok));
      g_free (calendar_tool_find (junk[i]));
      g_free (calendar_tool_change (junk[i], junk[i], junk[i], junk[i], &ok));
      g_free (calendar_tool_delete (junk[i], &ok));
      g_free (calendar_tool_done (junk[i], &ok));
      g_free (calendar_tool_free (junk[i], junk[i]));
    }
  g_autofree char *after = calendar_tool_agenda (day6);
  CHECK (strstr (after, "Dentist"), "junk did not damage the calendar: %s", after);

  i18n_set ("es");
  g_autofree char *es = calendar_tool_add ("cena con Ana viernes 8pm", &ok);
  CHECK (ok && strstr (es, "Apuntado:") && strstr (es, "20:00"), "Spanish: %s", es);

  calendar_default_free ();
  g_autofree char *file = g_build_filename (tmp, "npu-chat", "calendar.json", NULL);
  g_autofree char *dir = g_build_filename (tmp, "npu-chat", NULL);
  g_remove (file);
  g_rmdir (dir);
  g_rmdir (tmp);
  g_print (failures ? "test-tools: %d failure(s)\n" : "test-tools: all checks passed\n", failures);
  return failures ? 1 : 0;
}
