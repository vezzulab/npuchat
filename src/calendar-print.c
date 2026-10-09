#include "calendar-print.h"

#include <math.h>
#include <string.h>

#include "calendar-settings.h"
#include "calendar.h"
#include "i18n.h"

typedef struct {
  PrintView view;
  gint64    anchor;
} Job;

static const char *months_es[] = { "enero", "febrero", "marzo", "abril", "mayo", "junio", "julio", "agosto",
                                   "septiembre", "octubre", "noviembre", "diciembre" };
static const char *months_en[] = { "January", "February", "March", "April", "May", "June", "July", "August",
                                   "September", "October", "November", "December" };
static const char *days_es[] = { "lunes", "martes", "miércoles", "jueves", "viernes", "sábado", "domingo" };
static const char *days_en[] = { "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday" };

static gboolean
english (void)
{
  return i18n_lang () == LANG_EN;
}

static int
week_start (void)
{
  const CalSettings *cs = calendar_settings ();
  if (cs->days == 5)
    return 0;
  if (cs->first_day)
    return cs->first_day - 1;
  return english () ? 6 : 0;
}

static gint64
shift_days (gint64 t, int n)
{
  g_autoptr (GDateTime) d = g_date_time_new_from_unix_local (t);
  g_autoptr (GDateTime) r = g_date_time_add_days (d, n);
  return g_date_time_to_unix (r);
}

static gint64
month_first (gint64 t)
{
  g_autoptr (GDateTime) d = g_date_time_new_from_unix_local (t);
  g_autoptr (GDateTime) m = g_date_time_new_local (g_date_time_get_year (d), g_date_time_get_month (d), 1, 0, 0, 0);
  return g_date_time_to_unix (m);
}

static gint64
week_first (gint64 t)
{
  g_autoptr (GDateTime) d = g_date_time_new_from_unix_local (t);
  int back = (g_date_time_get_day_of_week (d) - 1 - week_start () + 7) % 7;
  return shift_days (calendar_day_start (t), -back);
}

/* ---- drawing helpers ---------------------------------------------------- */

static void
text (cairo_t *cr, PangoLayout *l, const char *s, double x, double y, double w, double h, double size, gboolean bold,
      PangoAlignment align, double gray)
{
  PangoFontDescription *fd = pango_font_description_new ();
  pango_font_description_set_family (fd, "Sans");
  pango_font_description_set_size (fd, (gint) (size * PANGO_SCALE));
  pango_font_description_set_weight (fd, bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
  pango_layout_set_font_description (l, fd);
  pango_layout_set_text (l, s, -1);
  pango_layout_set_width (l, (int) (w * PANGO_SCALE));
  pango_layout_set_height (l, (int) (h * PANGO_SCALE));
  pango_layout_set_ellipsize (l, PANGO_ELLIPSIZE_END);
  pango_layout_set_alignment (l, align);
  cairo_set_source_rgb (cr, gray, gray, gray);
  cairo_move_to (cr, x, y);
  pango_cairo_show_layout (cr, l);
  pango_font_description_free (fd);
}

static void
color (cairo_t *cr, guint c, double alpha)
{
  GdkRGBA rgba;
  gdk_rgba_parse (&rgba, calendar_color_hex (c));
  cairo_set_source_rgba (cr, rgba.red, rgba.green, rgba.blue, alpha);
}

static const CalCalendar *
cal_of (const CalEvent *ev)
{
  return calendar_calendar_find (calendar_default (), ev->calendar);
}

static char *
hm (gint64 t)
{
  g_autoptr (GDateTime) d = g_date_time_new_from_unix_local (t);
  return g_date_time_format (d, "%H:%M");
}

static char *
title_for (const Job *j)
{
  g_autoptr (GDateTime) d = g_date_time_new_from_unix_local (j->anchor);
  int m = g_date_time_get_month (d) - 1, y = g_date_time_get_year (d);
  const char **ms = english () ? months_en : months_es;
  if (j->view == PRINT_YEAR)
    return g_strdup_printf ("%d", y);
  if (j->view == PRINT_MONTH)
    {
      char *s = g_strdup_printf ("%s %d", ms[m], y);
      s[0] = (char) g_ascii_toupper (s[0]);
      return s;
    }
  if (j->view == PRINT_DAY)
    {
      int dow = g_date_time_get_day_of_week (d) - 1;
      char *s = english () ? g_strdup_printf ("%s, %s %d, %d", days_en[dow], ms[m], g_date_time_get_day_of_month (d), y)
                           : g_strdup_printf ("%s %d de %s de %d", days_es[dow], g_date_time_get_day_of_month (d), ms[m], y);
      s[0] = (char) g_ascii_toupper (s[0]);
      return s;
    }
  gint64 a = week_first (j->anchor), b = shift_days (a, calendar_settings ()->days - 1);
  g_autoptr (GDateTime) da = g_date_time_new_from_unix_local (a);
  g_autoptr (GDateTime) db = g_date_time_new_from_unix_local (b);
  return g_strdup_printf ("%d %s – %d %s %d", g_date_time_get_day_of_month (da), ms[g_date_time_get_month (da) - 1],
                          g_date_time_get_day_of_month (db), ms[g_date_time_get_month (db) - 1], g_date_time_get_year (db));
}

/* ---- month -------------------------------------------------------------- */

static void
draw_month (cairo_t *cr, PangoLayout *l, double x0, double y0, double w, double h, const Job *j)
{
  int ncols = calendar_settings ()->days;
  gint64 m0 = month_first (j->anchor);
  g_autoptr (GDateTime) first = g_date_time_new_from_unix_local (m0);
  int offset = (g_date_time_get_day_of_week (first) - 1 - week_start () + 7) % 7;
  gint64 start = shift_days (m0, -offset);
  GArray *occ = calendar_occurrences (calendar_default (), start, shift_days (start, 42));
  double head = 18, cw = w / ncols, ch = (h - head) / 6;
  for (int i = 0; i < ncols; i++)
    {
      int dow = (week_start () + i) % 7;
      text (cr, l, english () ? days_en[dow] : days_es[dow], x0 + i * cw + 4, y0, cw - 8, head, 8, TRUE, PANGO_ALIGN_LEFT, 0.35);
    }
  for (int i = 0; i < 42; i++)
    {
      gint64 day = shift_days (start, i), next = calendar_day_next (day);
      g_autoptr (GDateTime) d = g_date_time_new_from_unix_local (day);
      int dow = g_date_time_get_day_of_week (d) - 1;
      int col = ncols == 5 ? dow : i % 7;
      if (ncols == 5 && dow >= 5)
        continue;
      double cx = x0 + col * cw, cy = y0 + head + (i / 7) * ch;
      cairo_set_line_width (cr, 0.5);
      cairo_set_source_rgb (cr, 0.6, 0.6, 0.6);
      cairo_rectangle (cr, cx, cy, cw, ch);
      cairo_stroke (cr);
      gboolean in_month = g_date_time_get_month (d) == g_date_time_get_month (first);
      g_autofree char *num = g_strdup_printf ("%d", g_date_time_get_day_of_month (d));
      text (cr, l, num, cx + 3, cy + 2, cw - 6, 12, 9, TRUE, PANGO_ALIGN_RIGHT, in_month ? 0.1 : 0.6);
      double ey = cy + 15;
      for (guint k = 0; k < occ->len && ey < cy + ch - 8; k++)
        {
          const CalOccurrence *o = &g_array_index (occ, CalOccurrence, k);
          if (o->start >= next || o->end <= day || !cal_of (o->event)->visible)
            continue;
          if (o->event->all_day || o->start < day)
            {
              color (cr, cal_of (o->event)->color, 0.25);
              cairo_rectangle (cr, cx + 2, ey, cw - 4, 9);
              cairo_fill (cr);
            }
          g_autofree char *when = o->event->all_day || o->start < day ? NULL : hm (o->start);
          g_autofree char *line = when ? g_strdup_printf ("%s %s", when, o->event->title) : g_strdup (o->event->title);
          text (cr, l, line, cx + 4, ey, cw - 8, 9, 7, FALSE, PANGO_ALIGN_LEFT, 0.1);
          ey += 9.5;
        }
    }
  g_array_free (occ, TRUE);
}

/* ---- week and day ------------------------------------------------------- */

typedef struct {
  const CalOccurrence *o;
  gint64 s, e;
  int col, cols;
} Slot;

static int
slot_cmp (gconstpointer a, gconstpointer b)
{
  const Slot *x = a, *y = b;
  if (x->s != y->s)
    return x->s < y->s ? -1 : 1;
  return x->e > y->e ? -1 : (x->e < y->e);
}

static void
draw_time_grid (cairo_t *cr, PangoLayout *l, double x0, double y0, double w, double h, const Job *j)
{
  int n = j->view == PRINT_DAY ? 1 : calendar_settings ()->days;
  gint64 first = j->view == PRINT_DAY ? calendar_day_start (j->anchor) : week_first (j->anchor);
  gint64 end = shift_days (first, n);
  const CalSettings *cs = calendar_settings ();
  int h0 = MAX (cs->day_start - 1, 0), h1 = MIN (cs->day_end + 1, 24);
  double gutter = 34, head = 30, strip = 0;
  GArray *occ = calendar_occurrences (calendar_default (), first, end);
  double cw = (w - gutter) / n;

  /* all-day events in a strip under the day names */
  int rows = 0;
  for (int i = 0; i < n; i++)
    {
      gint64 day = shift_days (first, i), next = calendar_day_next (day);
      int row = 0;
      for (guint k = 0; k < occ->len; k++)
        {
          const CalOccurrence *o = &g_array_index (occ, CalOccurrence, k);
          if (!o->event->all_day || o->start >= next || o->end <= day || !cal_of (o->event)->visible)
            continue;
          color (cr, cal_of (o->event)->color, 0.25);
          cairo_rectangle (cr, x0 + gutter + i * cw + 1, y0 + head + row * 11, cw - 2, 10);
          cairo_fill (cr);
          text (cr, l, o->event->title, x0 + gutter + i * cw + 3, y0 + head + row * 11, cw - 6, 10, 7.5, FALSE, PANGO_ALIGN_LEFT, 0.1);
          row++;
        }
      rows = MAX (rows, row);
    }
  strip = rows * 11 + (rows ? 4 : 0);

  double top = y0 + head + strip, gh = h - head - strip;
  double hour_h = gh / (h1 - h0);
  for (int i = 0; i < n; i++)
    {
      gint64 day = shift_days (first, i);
      g_autoptr (GDateTime) d = g_date_time_new_from_unix_local (day);
      int dow = g_date_time_get_day_of_week (d) - 1;
      g_autofree char *label = g_strdup_printf ("%s %d", english () ? days_en[dow] : days_es[dow], g_date_time_get_day_of_month (d));
      label[0] = (char) g_ascii_toupper (label[0]);
      text (cr, l, label, x0 + gutter + i * cw, y0 + 6, cw, 16, 9, TRUE, PANGO_ALIGN_CENTER, 0.1);
      cairo_set_line_width (cr, 0.5);
      cairo_set_source_rgb (cr, 0.7, 0.7, 0.7);
      cairo_move_to (cr, x0 + gutter + i * cw, top);
      cairo_line_to (cr, x0 + gutter + i * cw, top + gh);
      cairo_stroke (cr);
    }
  cairo_move_to (cr, x0 + gutter + n * cw, top);
  cairo_line_to (cr, x0 + gutter + n * cw, top + gh);
  cairo_stroke (cr);
  for (int hr = h0; hr <= h1; hr++)
    {
      double y = top + (hr - h0) * hour_h;
      cairo_set_source_rgb (cr, 0.75, 0.75, 0.75);
      cairo_move_to (cr, x0 + gutter, y);
      cairo_line_to (cr, x0 + w, y);
      cairo_stroke (cr);
      if (hr < h1)
        {
          g_autofree char *lab = g_strdup_printf ("%02d:00", hr);
          text (cr, l, lab, x0, y - 1, gutter - 4, 10, 7, FALSE, PANGO_ALIGN_RIGHT, 0.4);
        }
    }

  for (int i = 0; i < n; i++)
    {
      gint64 day = shift_days (first, i), next = calendar_day_next (day);
      GArray *slots = g_array_new (FALSE, FALSE, sizeof (Slot));
      for (guint k = 0; k < occ->len; k++)
        {
          const CalOccurrence *o = &g_array_index (occ, CalOccurrence, k);
          if (o->event->all_day || o->start >= next || o->end <= day || !cal_of (o->event)->visible)
            continue;
          Slot s = { o, MAX (o->start, day), MIN (o->end, next), 0, 1 };
          if (s.e - s.s < 20 * 60)
            s.e = s.s + 20 * 60;
          g_array_append_val (slots, s);
        }
      g_array_sort (slots, slot_cmp);
      guint a = 0;
      while (a < slots->len)
        {
          gint64 cluster_end = g_array_index (slots, Slot, a).e;
          guint b = a;
          GArray *col_end = g_array_new (FALSE, FALSE, sizeof (gint64));
          while (b < slots->len && g_array_index (slots, Slot, b).s < cluster_end)
            {
              Slot *s = &g_array_index (slots, Slot, b);
              guint c = 0;
              while (c < col_end->len && g_array_index (col_end, gint64, c) > s->s)
                c++;
              if (c == col_end->len)
                g_array_append_val (col_end, s->e);
              else
                g_array_index (col_end, gint64, c) = s->e;
              s->col = (int) c;
              cluster_end = MAX (cluster_end, s->e);
              b++;
            }
          for (guint k = a; k < b; k++)
            g_array_index (slots, Slot, k).cols = (int) col_end->len;
          g_array_free (col_end, TRUE);
          a = b;
        }
      for (guint k = 0; k < slots->len; k++)
        {
          const Slot *s = &g_array_index (slots, Slot, k);
          g_autoptr (GDateTime) sd = g_date_time_new_from_unix_local (s->s);
          double sy = top + ((g_date_time_get_hour (sd) + g_date_time_get_minute (sd) / 60.0) - h0) * hour_h;
          double eh = (double) (s->e - s->s) / 3600.0 * hour_h;
          if (sy < top - 1 || sy > top + gh)
            continue;
          double bw = (cw - 3) / s->cols, bx = x0 + gutter + i * cw + 1 + s->col * bw;
          color (cr, cal_of (s->o->event)->color, 0.25);
          cairo_rectangle (cr, bx, sy, bw - 1, MIN (eh, top + gh - sy));
          cairo_fill (cr);
          color (cr, cal_of (s->o->event)->color, 1);
          cairo_rectangle (cr, bx, sy, 2, MIN (eh, top + gh - sy));
          cairo_fill (cr);
          g_autofree char *when = hm (s->o->start);
          g_autofree char *line = g_strdup_printf ("%s %s", when, s->o->event->title);
          text (cr, l, line, bx + 4, sy + 1, bw - 6, MIN (eh, 20), 7.5, FALSE, PANGO_ALIGN_LEFT, 0.1);
        }
      g_array_free (slots, TRUE);
    }
  g_array_free (occ, TRUE);
}

/* ---- year --------------------------------------------------------------- */

static void
draw_year (cairo_t *cr, PangoLayout *l, double x0, double y0, double w, double h, const Job *j)
{
  g_autoptr (GDateTime) a = g_date_time_new_from_unix_local (j->anchor);
  int year = g_date_time_get_year (a);
  double cw = w / 4, ch = h / 3;
  for (int m = 1; m <= 12; m++)
    {
      double mx = x0 + ((m - 1) % 4) * cw + 6, my = y0 + ((m - 1) / 4) * ch;
      g_autoptr (GDateTime) f = g_date_time_new_local (year, m, 1, 0, 0, 0);
      const char *name = (english () ? months_en : months_es)[m - 1];
      g_autofree char *title = g_strdup (name);
      title[0] = (char) g_ascii_toupper (title[0]);
      text (cr, l, title, mx, my, cw - 12, 14, 10, TRUE, PANGO_ALIGN_LEFT, 0.1);
      double dw = (cw - 12) / 7, dh = (ch - 40) / 7;
      for (int i = 0; i < 7; i++)
        {
          int dow = (week_start () + i) % 7;
          g_autofree char *letter = g_strndup (english () ? days_en[dow] : days_es[dow], 1);
          letter[0] = (char) g_ascii_toupper (letter[0]);
          text (cr, l, letter, mx + i * dw, my + 18, dw, dh, 6.5, TRUE, PANGO_ALIGN_CENTER, 0.45);
        }
      int offset = (g_date_time_get_day_of_week (f) - 1 - week_start () + 7) % 7;
      int dim = g_date_get_days_in_month ((GDateMonth) m, (GDateYear) year);
      for (int d = 1; d <= dim; d++)
        {
          int idx = offset + d - 1;
          g_autofree char *num = g_strdup_printf ("%d", d);
          text (cr, l, num, mx + (idx % 7) * dw, my + 18 + (1 + idx / 7) * dh, dw, dh, 7, FALSE, PANGO_ALIGN_CENTER, 0.1);
        }
    }
}

/* ---- the print operation ------------------------------------------------ */

static void
on_begin (GtkPrintOperation *op, GtkPrintContext *ctx, gpointer data)
{
  (void) ctx; (void) data;
  gtk_print_operation_set_n_pages (op, 1);
}

static void
on_draw (GtkPrintOperation *op, GtkPrintContext *ctx, int page, gpointer data)
{
  (void) op; (void) page;
  Job *j = data;
  cairo_t *cr = gtk_print_context_get_cairo_context (ctx);
  double w = gtk_print_context_get_width (ctx), h = gtk_print_context_get_height (ctx);
  PangoLayout *l = gtk_print_context_create_pango_layout (ctx);
  g_autofree char *title = title_for (j);
  text (cr, l, title, 0, 0, w, 28, 20, TRUE, PANGO_ALIGN_LEFT, 0.05);
  double top = 36;
  switch (j->view)
    {
    case PRINT_MONTH: draw_month (cr, l, 0, top, w, h - top, j); break;
    case PRINT_YEAR:  draw_year (cr, l, 0, top, w, h - top, j); break;
    default:          draw_time_grid (cr, l, 0, top, w, h - top, j); break;
    }
  g_object_unref (l);
}

void
calendar_print (GtkWindow *parent, PrintView view, gint64 anchor, const char *export_pdf)
{
  Job *j = g_new0 (Job, 1);
  j->view = view;
  j->anchor = anchor;
  GtkPrintOperation *op = gtk_print_operation_new ();
  gtk_print_operation_set_job_name (op, "Calendar");
  g_signal_connect (op, "begin-print", G_CALLBACK (on_begin), NULL);
  g_signal_connect (op, "draw-page", G_CALLBACK (on_draw), j);
  g_autoptr (GtkPageSetup) setup = gtk_page_setup_new ();
  gtk_page_setup_set_orientation (setup, view == PRINT_DAY ? GTK_PAGE_ORIENTATION_PORTRAIT : GTK_PAGE_ORIENTATION_LANDSCAPE);
  gtk_print_operation_set_default_page_setup (op, setup);
  if (export_pdf)
    gtk_print_operation_set_export_filename (op, export_pdf);
  g_autoptr (GError) error = NULL;
  gtk_print_operation_run (op, export_pdf ? GTK_PRINT_OPERATION_ACTION_EXPORT : GTK_PRINT_OPERATION_ACTION_PRINT_DIALOG, parent, &error);
  if (error)
    g_warning ("calendar: printing failed: %s", error->message);
  g_object_unref (op);
  g_free (j);
}
