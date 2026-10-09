#include <glib.h>

#include "../src/calendar-quick.h"

/* "now" is Thursday 8 October 2026, 10:00 */
static gint64
at (int y, int mo, int d, int h, int mi)
{
  g_autoptr (GDateTime) dt = g_date_time_new_local (y, mo, d, h, mi, 0);
  return g_date_time_to_unix (dt);
}

static void
check (const char *text, const char *title, gint64 start, gint64 end, gboolean all_day, CalRepeat repeat, gboolean month_first)
{
  CalQuick q;
  g_assert_true (calendar_quick_parse (text, at (2026, 10, 8, 10, 0), month_first, &q));
  gboolean ok = g_str_equal (q.title, title) && q.start == start && q.end == end && q.all_day == all_day && q.repeat == repeat;
  if (!ok)
    {
      g_autoptr (GDateTime) s = g_date_time_new_from_unix_local (q.start);
      g_autoptr (GDateTime) e = g_date_time_new_from_unix_local (q.end);
      g_autofree char *ss = g_date_time_format (s, "%Y-%m-%d %H:%M");
      g_autofree char *es = g_date_time_format (e, "%Y-%m-%d %H:%M");
      g_autoptr (GDateTime) ws = g_date_time_new_from_unix_local (start);
      g_autoptr (GDateTime) we = g_date_time_new_from_unix_local (end);
      g_autofree char *wss = g_date_time_format (ws, "%Y-%m-%d %H:%M");
      g_autofree char *wes = g_date_time_format (we, "%Y-%m-%d %H:%M");
      g_error ("\"%s\"\n   got  title=\"%s\" %s -> %s all_day=%d repeat=%d\n   want title=\"%s\" %s -> %s all_day=%d repeat=%d",
               text, q.title, ss, es, q.all_day, q.repeat, title, wss, wes, all_day, repeat);
    }
  calendar_quick_clear (&q);
}

static void
test_spanish (void)
{
  check ("cena con Ana jueves 7pm", "Cena con Ana", at (2026, 10, 8, 19, 0), at (2026, 10, 8, 20, 0), FALSE, CAL_REPEAT_NONE, FALSE);
  check ("dentista mañana 3pm", "Dentista", at (2026, 10, 9, 15, 0), at (2026, 10, 9, 16, 0), FALSE, CAL_REPEAT_NONE, FALSE);
  check ("reunión viernes de 10 a 11", "Reunión", at (2026, 10, 9, 10, 0), at (2026, 10, 9, 11, 0), FALSE, CAL_REPEAT_NONE, FALSE);
  check ("gimnasio cada lunes 6am", "Gimnasio", at (2026, 10, 12, 6, 0), at (2026, 10, 12, 7, 0), FALSE, CAL_REPEAT_WEEKLY, FALSE);
  check ("Fiesta 6 de noviembre", "Fiesta", at (2026, 11, 6, 0, 0), at (2026, 11, 7, 0, 0), TRUE, CAL_REPEAT_NONE, FALSE);
  check ("almuerzo con mamá hoy a las 2", "Almuerzo con mamá", at (2026, 10, 8, 14, 0), at (2026, 10, 8, 15, 0), FALSE, CAL_REPEAT_NONE, FALSE);
  check ("llamar a Juan el 15", "Llamar a Juan", at (2026, 10, 15, 0, 0), at (2026, 10, 16, 0, 0), TRUE, CAL_REPEAT_NONE, FALSE);
  check ("yoga todos los días 7am", "Yoga", at (2026, 10, 8, 7, 0), at (2026, 10, 8, 8, 0), FALSE, CAL_REPEAT_DAILY, FALSE);
  check ("clase de inglés martes 8 de la noche", "Clase de inglés", at (2026, 10, 13, 20, 0), at (2026, 10, 13, 21, 0), FALSE, CAL_REPEAT_NONE, FALSE);
  check ("doctor en 3 días 9:30", "Doctor", at (2026, 10, 11, 9, 30), at (2026, 10, 11, 10, 30), FALSE, CAL_REPEAT_NONE, FALSE);
  check ("cumpleaños de Luis 25/12 todos los años", "Cumpleaños de Luis", at (2026, 12, 25, 0, 0), at (2026, 12, 26, 0, 0), TRUE, CAL_REPEAT_YEARLY, FALSE);
  check ("cena con Ana el viernes a las 8", "Cena con Ana", at (2026, 10, 9, 20, 0), at (2026, 10, 9, 21, 0), FALSE, CAL_REPEAT_NONE, FALSE);
  check ("reunión el viernes a las 8", "Reunión", at (2026, 10, 9, 8, 0), at (2026, 10, 9, 9, 0), FALSE, CAL_REPEAT_NONE, FALSE);
  check ("llamar a mamá el sábado 10:00", "Llamar a mamá", at (2026, 10, 10, 10, 0), at (2026, 10, 10, 11, 0), FALSE, CAL_REPEAT_NONE, FALSE);
  check ("Comprar regalos viernes 16 de octubre", "Comprar regalos", at (2026, 10, 16, 0, 0), at (2026, 10, 17, 0, 0), TRUE, CAL_REPEAT_NONE, FALSE);
  check ("regalos jueves, 15 de octubre de 2026", "Regalos", at (2026, 10, 15, 0, 0), at (2026, 10, 16, 0, 0), TRUE, CAL_REPEAT_NONE, FALSE);
  check ("regalos 15 de octubre 2026", "Regalos", at (2026, 10, 15, 0, 0), at (2026, 10, 16, 0, 0), TRUE, CAL_REPEAT_NONE, FALSE);
  check ("regalos para Evelyn el día 15", "Regalos para Evelyn", at (2026, 10, 15, 0, 0), at (2026, 10, 16, 0, 0), TRUE, CAL_REPEAT_NONE, FALSE);
  check ("entrega pasado mañana", "Entrega", at (2026, 10, 10, 0, 0), at (2026, 10, 11, 0, 0), TRUE, CAL_REPEAT_NONE, FALSE);
  check ("médico el próximo martes a las 11 de la mañana", "Médico", at (2026, 10, 13, 11, 0), at (2026, 10, 13, 12, 0), FALSE, CAL_REPEAT_NONE, FALSE);
}

static void
test_english (void)
{
  check ("gifts friday october 16 2026", "Gifts", at (2026, 10, 16, 0, 0), at (2026, 10, 17, 0, 0), TRUE, CAL_REPEAT_NONE, TRUE);
  check ("lunch with Sam tomorrow at noon", "Lunch with Sam", at (2026, 10, 9, 12, 0), at (2026, 10, 9, 13, 0), FALSE, CAL_REPEAT_NONE, TRUE);
  check ("meeting friday from 2 to 3pm", "Meeting", at (2026, 10, 9, 14, 0), at (2026, 10, 9, 15, 0), FALSE, CAL_REPEAT_NONE, TRUE);
  check ("dentist oct 20 4pm", "Dentist", at (2026, 10, 20, 16, 0), at (2026, 10, 20, 17, 0), FALSE, CAL_REPEAT_NONE, TRUE);
  check ("call mom next monday 6:30pm", "Call mom", at (2026, 10, 12, 18, 30), at (2026, 10, 12, 19, 30), FALSE, CAL_REPEAT_NONE, TRUE);
  check ("gym every monday 6am", "Gym", at (2026, 10, 12, 6, 0), at (2026, 10, 12, 7, 0), FALSE, CAL_REPEAT_WEEKLY, TRUE);
  check ("birthday party saturday for 3 hours 5pm", "Birthday party", at (2026, 10, 10, 17, 0), at (2026, 10, 10, 20, 0), FALSE, CAL_REPEAT_NONE, TRUE);
  check ("review 10/20 11am", "Review", at (2026, 10, 20, 11, 0), at (2026, 10, 20, 12, 0), FALSE, CAL_REPEAT_NONE, TRUE);
  check ("Party Feb 6", "Party", at (2027, 2, 6, 0, 0), at (2027, 2, 7, 0, 0), TRUE, CAL_REPEAT_NONE, TRUE);
  check ("workout tomorrow 7am 1h30", "Workout", at (2026, 10, 9, 7, 0), at (2026, 10, 9, 8, 30), FALSE, CAL_REPEAT_NONE, TRUE);
  check ("dinner with Sam friday at 8", "Dinner with Sam", at (2026, 10, 9, 20, 0), at (2026, 10, 9, 21, 0), FALSE, CAL_REPEAT_NONE, TRUE);
  check ("meeting at 3", "Meeting", at (2026, 10, 8, 15, 0), at (2026, 10, 8, 16, 0), FALSE, CAL_REPEAT_NONE, TRUE);
  check ("standup at 9am", "Standup", at (2026, 10, 9, 9, 0), at (2026, 10, 9, 10, 0), FALSE, CAL_REPEAT_NONE, TRUE);
  check ("coffee with Dana thursday 10-11am", "Coffee with Dana", at (2026, 10, 8, 10, 0), at (2026, 10, 8, 11, 0), FALSE, CAL_REPEAT_NONE, TRUE);
  check ("flight to Madrid in 2 weeks", "Flight to Madrid", at (2026, 10, 22, 0, 0), at (2026, 10, 23, 0, 0), TRUE, CAL_REPEAT_NONE, TRUE);
}

static void
check_extras (const char *text, gboolean reminder, int alert, guint interval, int count, CalRepeat repeat, gboolean month_first)
{
  CalQuick q;
  g_assert_true (calendar_quick_parse (text, at (2026, 10, 8, 10, 0), month_first, &q));
  if (q.reminder != reminder || q.alert != alert || q.interval != interval || q.count != count || q.repeat != repeat)
    g_error ("\"%s\"\n   got reminder=%d alert=%d interval=%u count=%d repeat=%d title=\"%s\"\n   want reminder=%d alert=%d interval=%u count=%d repeat=%d",
             text, q.reminder, q.alert, q.interval, q.count, q.repeat, q.title, reminder, alert, interval, count, repeat);
  calendar_quick_clear (&q);
}

static void
test_extras (void)
{
  CalQuick q;
  check_extras ("dentista mañana 3pm", FALSE, -1, 0, 0, CAL_REPEAT_NONE, FALSE);
  check_extras ("dentista mañana 3pm avísame 15 min antes", FALSE, 15, 0, 0, CAL_REPEAT_NONE, FALSE);
  check_extras ("flight friday 8am alert 2 hours before", FALSE, 120, 0, 0, CAL_REPEAT_NONE, TRUE);
  check_extras ("cumpleaños de mamá 3 de noviembre 1 día antes", FALSE, 1440, 0, 0, CAL_REPEAT_NONE, FALSE);
  check_extras ("recuérdame pagar la renta mañana", TRUE, -1, 0, 0, CAL_REPEAT_NONE, FALSE);
  check_extras ("remind me to call mom friday 5pm", TRUE, -1, 0, 0, CAL_REPEAT_NONE, TRUE);
  check_extras ("gym cada 2 semanas lunes 6am", FALSE, -1, 2, 0, CAL_REPEAT_WEEKLY, FALSE);
  check_extras ("gym every other week monday 6am", FALSE, -1, 2, 0, CAL_REPEAT_WEEKLY, TRUE);
  check_extras ("curso cada semana 8 veces jueves 6pm", FALSE, -1, 0, 8, CAL_REPEAT_WEEKLY, FALSE);
  check_extras ("rent every 3 months on the 1st", FALSE, -1, 3, 0, CAL_REPEAT_MONTHLY, TRUE);
  check_extras ("pills every day 5 times 8am", FALSE, -1, 0, 5, CAL_REPEAT_DAILY, TRUE);

  /* an ISO date, as models and files write it */
  g_assert_true (calendar_quick_parse ("dentist 2026-11-20 16:30", at (2026, 10, 8, 10, 0), TRUE, &q));
  g_assert_cmpint (q.start, ==, at (2026, 11, 20, 16, 30));
  g_assert_cmpstr (q.title, ==, "Dentist");
  calendar_quick_clear (&q);

  /* titles come out clean */
  g_assert_true (calendar_quick_parse ("recuérdame pagar la renta mañana", at (2026, 10, 8, 10, 0), FALSE, &q));
  g_assert_cmpstr (q.title, ==, "Pagar la renta");
  calendar_quick_clear (&q);
  g_assert_true (calendar_quick_parse ("dentista mañana 3pm avísame 15 min antes", at (2026, 10, 8, 10, 0), FALSE, &q));
  g_assert_cmpstr (q.title, ==, "Dentista");
  calendar_quick_clear (&q);
  g_assert_true (calendar_quick_parse ("curso cada semana hasta el 15 de diciembre jueves 6pm", at (2026, 10, 8, 10, 0), FALSE, &q));
  g_assert_cmpstr (q.title, ==, "Curso");
  g_assert_cmpint (q.until, ==, at (2026, 12, 15, 0, 0));
  g_assert_cmpint (q.start, ==, at (2026, 10, 8, 18, 0));
  calendar_quick_clear (&q);
  g_assert_true (calendar_quick_parse ("class every week until december 15 thursday 6pm", at (2026, 10, 8, 10, 0), TRUE, &q));
  g_assert_cmpint (q.until, ==, at (2026, 12, 15, 0, 0));
  g_assert_cmpstr (q.title, ==, "Class");
  calendar_quick_clear (&q);
}

static void
test_edges (void)
{
  CalQuick q;
  g_assert_false (calendar_quick_parse ("", 0, FALSE, &q));
  g_assert_false (calendar_quick_parse (NULL, 0, FALSE, &q));
  /* only a date: the title is left empty for the caller to fill in */
  g_assert_true (calendar_quick_parse ("mañana", at (2026, 10, 8, 10, 0), FALSE, &q));
  g_assert_cmpstr (q.title, ==, "");
  g_assert_true (q.all_day);
  calendar_quick_clear (&q);
  /* numbers that are part of the title are not times */
  g_assert_true (calendar_quick_parse ("estudiar capítulo 5 viernes", at (2026, 10, 8, 10, 0), FALSE, &q));
  g_assert_cmpstr (q.title, ==, "Estudiar capítulo 5");
  g_assert_true (q.all_day);
  calendar_quick_clear (&q);
  /* never loops or crashes on junk */
  const char *junk[] = { "- - -", "a las", "de 10 a", "99/99/9999", "el 99", "por horas", "@", "3pm 4pm 5pm 6pm", "cada", "25:99" };
  for (guint i = 0; i < G_N_ELEMENTS (junk); i++)
    {
      calendar_quick_parse (junk[i], at (2026, 10, 8, 10, 0), FALSE, &q);
      g_assert_cmpint (q.end, >, q.start);
      calendar_quick_clear (&q);
    }
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/quick/spanish", test_spanish);
  g_test_add_func ("/quick/english", test_english);
  g_test_add_func ("/quick/extras", test_extras);
  g_test_add_func ("/quick/edges", test_edges);
  return g_test_run ();
}
