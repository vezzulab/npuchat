#include "calendar-grid.h"

#include <math.h>
#include <string.h>

#include "calendar.h"
#include "i18n.h"

#define HOUR_H 56.0
#define GUTTER 54.0
#define SNAP_MIN 15
#define EDGE 7.0           /* the bottom strip of an event that resizes it */
#define MIN_DRAG 4.0       /* pixels before a press becomes a drag */

typedef struct {
  double x, y, w, h;
  char  *id;
  gint64 occ_start;        /* start of this showing of the event */
  guint  color;
} Block;

typedef enum { DRAG_NONE, DRAG_CREATE, DRAG_MOVE, DRAG_RESIZE } DragKind;

typedef struct {
  GtkWidget          *area;
  CalGridCallbacks    cb;
  gpointer            data;
  gint64              first_day;
  int                 ndays;
  GArray             *blocks;      /* Block, rebuilt on every draw */
  guint               clock_id;    /* minute timer, only while the widget is on screen */

  DragKind            drag;
  double              press_x, press_y;
  gboolean            moved;
  char               *drag_id;     /* event being moved or resized */
  gint64              drag_occ_start, drag_start, drag_end;   /* the event as it was */
  int                 drag_grab_min;                          /* minutes between the event's top and the press */
  /* proposed result while dragging */
  gint64              ghost_start, ghost_end;
  char               *selected;
} Grid;

static void
block_clear (gpointer p)
{
  g_free (((Block *) p)->id);
}

/* ---- time <-> pixels ---------------------------------------------------- */

static gint64
day_at (const Grid *g, int index)
{
  g_autoptr (GDateTime) d = g_date_time_new_from_unix_local (g->first_day);
  g_autoptr (GDateTime) r = g_date_time_add_days (d, index);
  return g_date_time_to_unix (r);
}

static int
minutes_in_day (gint64 t)
{
  g_autoptr (GDateTime) d = g_date_time_new_from_unix_local (t);
  return g_date_time_get_hour (d) * 60 + g_date_time_get_minute (d);
}

static gint64
time_at (gint64 day, int minutes)
{
  g_autoptr (GDateTime) d = g_date_time_new_from_unix_local (day);
  g_autoptr (GDateTime) r = g_date_time_new_local (g_date_time_get_year (d), g_date_time_get_month (d),
                                                    g_date_time_get_day_of_month (d), 0, 0, 0);
  g_autoptr (GDateTime) t = g_date_time_add_minutes (r, minutes);
  return g_date_time_to_unix (t);
}

static double
minutes_to_y (int minutes)
{
  return minutes / 60.0 * HOUR_H;
}

static int
y_to_minutes (double y, gboolean snap)
{
  int m = (int) floor (y / HOUR_H * 60.0 + 0.5);
  if (snap)
    m = (m + SNAP_MIN / 2) / SNAP_MIN * SNAP_MIN;
  return CLAMP (m, 0, 24 * 60);
}

static double
column_width (const Grid *g)
{
  double w = gtk_widget_get_width (g->area) - GUTTER;
  return w > 0 ? w / g->ndays : 1;
}

static int
x_to_column (const Grid *g, double x)
{
  int c = (int) floor ((x - GUTTER) / column_width (g));
  return CLAMP (c, 0, g->ndays - 1);
}

/* ---- drawing helpers ---------------------------------------------------- */

static void
rounded (cairo_t *cr, double x, double y, double w, double h, double r)
{
  r = MIN (r, MIN (w, h) / 2);
  cairo_new_sub_path (cr);
  cairo_arc (cr, x + w - r, y + r, r, -G_PI / 2, 0);
  cairo_arc (cr, x + w - r, y + h - r, r, 0, G_PI / 2);
  cairo_arc (cr, x + r, y + h - r, r, G_PI / 2, G_PI);
  cairo_arc (cr, x + r, y + r, r, G_PI, 3 * G_PI / 2);
  cairo_close_path (cr);
}

static void
set_hex (cairo_t *cr, const char *hex, double alpha)
{
  GdkRGBA c;
  gdk_rgba_parse (&c, hex);
  cairo_set_source_rgba (cr, c.red, c.green, c.blue, alpha);
}

static void
draw_text (cairo_t *cr, GtkWidget *w, const char *text, double x, double y, double width, double height, gboolean bold,
           double size_pt, const GdkRGBA *color)
{
  PangoLayout *l = gtk_widget_create_pango_layout (w, text);
  PangoFontDescription *fd = pango_font_description_new ();
  pango_font_description_set_size (fd, (gint) (size_pt * PANGO_SCALE));
  pango_font_description_set_weight (fd, bold ? PANGO_WEIGHT_SEMIBOLD : PANGO_WEIGHT_NORMAL);
  pango_layout_set_font_description (l, fd);
  pango_layout_set_width (l, (int) (width * PANGO_SCALE));
  pango_layout_set_height (l, (int) (height * PANGO_SCALE));
  pango_layout_set_ellipsize (l, PANGO_ELLIPSIZE_END);
  cairo_set_source_rgba (cr, color->red, color->green, color->blue, color->alpha);
  cairo_move_to (cr, x, y);
  pango_cairo_show_layout (cr, l);
  pango_font_description_free (fd);
  g_object_unref (l);
}

static char *
hour_label (int hour)
{
  if (i18n_lang () == LANG_EN)
    return hour == 0 ? g_strdup ("12 AM") : hour < 12 ? g_strdup_printf ("%d AM", hour) : hour == 12 ? g_strdup ("12 PM")
                                                                                                      : g_strdup_printf ("%d PM", hour - 12);
  return g_strdup_printf ("%02d:00", hour);
}

static char *
span_label (gint64 a, gint64 b)
{
  g_autoptr (GDateTime) da = g_date_time_new_from_unix_local (a);
  g_autoptr (GDateTime) db = g_date_time_new_from_unix_local (b);
  g_autofree char *sa = g_date_time_format (da, "%H:%M");
  g_autofree char *sb = g_date_time_format (db, "%H:%M");
  return g_strdup_printf ("%s – %s", sa, sb);
}

/* ---- layout: events that overlap share the column ----------------------- */

typedef struct {
  const CalOccurrence *occ;
  gint64 s, e;     /* clipped to the day */
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
layout_day (Grid *g, GArray *occ, int index)
{
  gint64 day = day_at (g, index), next = calendar_day_next (day);
  GArray *slots = g_array_new (FALSE, FALSE, sizeof (Slot));
  Calendar *cal = calendar_default ();
  for (guint i = 0; i < occ->len; i++)
    {
      const CalOccurrence *o = &g_array_index (occ, CalOccurrence, i);
      if (o->event->all_day || o->end <= day || o->start >= next)
        continue;
      if (!calendar_calendar_find (cal, o->event->calendar)->visible)
        continue;
      Slot s = { o, MAX (o->start, day), MIN (o->end, next), 0, 1 };
      if (s.e - s.s < 15 * 60)
        s.e = s.s + 15 * 60;
      g_array_append_val (slots, s);
    }
  g_array_sort (slots, slot_cmp);

  /* walk the events in order; a cluster ends when something starts after all of it */
  guint i = 0;
  while (i < slots->len)
    {
      gint64 cluster_end = g_array_index (slots, Slot, i).e;
      guint j = i;
      GArray *col_end = g_array_new (FALSE, FALSE, sizeof (gint64));
      while (j < slots->len && g_array_index (slots, Slot, j).s < cluster_end)
        {
          Slot *s = &g_array_index (slots, Slot, j);
          guint c = 0;
          while (c < col_end->len && g_array_index (col_end, gint64, c) > s->s)
            c++;
          if (c == col_end->len)
            g_array_append_val (col_end, s->e);
          else
            g_array_index (col_end, gint64, c) = s->e;
          s->col = (int) c;
          cluster_end = MAX (cluster_end, s->e);
          j++;
        }
      for (guint k = i; k < j; k++)
        g_array_index (slots, Slot, k).cols = (int) col_end->len;
      g_array_free (col_end, TRUE);
      i = j;
    }

  double cw = column_width (g);
  for (guint k = 0; k < slots->len; k++)
    {
      const Slot *s = &g_array_index (slots, Slot, k);
      double w = (cw - 6) / s->cols;
      Block b;
      b.x = GUTTER + index * cw + 2 + s->col * w;
      b.w = w - 2;
      b.y = minutes_to_y (minutes_in_day (s->s));
      b.h = MAX ((double) (s->e - s->s) / 3600.0 * HOUR_H - 2, 18);
      if (s->e >= next)
        b.h = 24 * HOUR_H - b.y - 1;
      b.id = g_strdup (s->occ->event->id);
      b.occ_start = s->occ->start;
      b.color = calendar_calendar_find (cal, s->occ->event->calendar)->color;
      g_array_append_val (g->blocks, b);
    }
  g_array_free (slots, TRUE);
}

/* ---- drawing ------------------------------------------------------------ */

static void
draw_block (Grid *g, cairo_t *cr, const Block *b, const CalEvent *ev, gint64 s, gint64 e, gboolean ghost, const GdkRGBA *fg)
{
  const char *hex = calendar_color_hex (b->color);
  gboolean selected = !ghost && g->selected && g_str_equal (g->selected, b->id);
  rounded (cr, b->x, b->y, b->w, b->h, 6);
  set_hex (cr, hex, selected ? 0.95 : ghost ? 0.55 : 0.22);
  cairo_fill_preserve (cr);
  if (!selected)
    {
      set_hex (cr, hex, 0.55);
      cairo_set_line_width (cr, 1);
      cairo_stroke (cr);
      cairo_save (cr);
      rounded (cr, b->x, b->y, b->w, b->h, 6);
      cairo_clip (cr);
      set_hex (cr, hex, 1);
      cairo_rectangle (cr, b->x, b->y, 3.5, b->h);
      cairo_fill (cr);
      cairo_restore (cr);
    }
  else
    cairo_new_path (cr);

  GdkRGBA text = selected ? (GdkRGBA) { 1, 1, 1, 1 } : *fg;
  cairo_save (cr);
  cairo_rectangle (cr, b->x, b->y, b->w, b->h);
  cairo_clip (cr);
  draw_text (cr, g->area, ev->title, b->x + 8, b->y + 3, b->w - 12, 16, TRUE, 9.5, &text);
  if (b->h > 34)
    {
      g_autofree char *span = span_label (s, e);
      GdkRGBA dim = text;
      dim.alpha = 0.75;
      draw_text (cr, g->area, span, b->x + 8, b->y + 19, b->w - 12, 14, FALSE, 8.5, &dim);
    }
  cairo_restore (cr);
}

static void
draw_cb (GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
  (void) area;
  Grid *g = data;
  GdkRGBA fg;
  gtk_widget_get_color (g->area, &fg);
  double cw = column_width (g);
  gint64 now = g_get_real_time () / G_USEC_PER_SEC;

  /* today's column */
  for (int i = 0; i < g->ndays; i++)
    {
      gint64 day = day_at (g, i);
      if (now >= day && now < calendar_day_next (day))
        {
          cairo_set_source_rgba (cr, fg.red, fg.green, fg.blue, 0.045);
          cairo_rectangle (cr, GUTTER + i * cw, 0, cw, height);
          cairo_fill (cr);
        }
    }

  /* hour lines and labels */
  cairo_set_line_width (cr, 1);
  for (int h = 0; h < 24; h++)
    {
      double y = floor (h * HOUR_H) + 0.5;
      cairo_set_source_rgba (cr, fg.red, fg.green, fg.blue, h == 0 ? 0 : 0.12);
      cairo_move_to (cr, GUTTER - 6, y);
      cairo_line_to (cr, width, y);
      cairo_stroke (cr);
      if (h > 0)
        {
          g_autofree char *l = hour_label (h);
          GdkRGBA dim = fg;
          dim.alpha = 0.55;
          draw_text (cr, g->area, l, 0, y - 8, GUTTER - 12, 16, FALSE, 8.5, &dim);
        }
    }
  /* day separators */
  for (int i = 0; i <= g->ndays; i++)
    {
      cairo_set_source_rgba (cr, fg.red, fg.green, fg.blue, 0.1);
      cairo_move_to (cr, floor (GUTTER + i * cw) + 0.5, 0);
      cairo_line_to (cr, floor (GUTTER + i * cw) + 0.5, height);
      cairo_stroke (cr);
    }

  /* events */
  g_array_set_size (g->blocks, 0);
  gint64 first = g->first_day, last = day_at (g, g->ndays);
  GArray *occ = calendar_occurrences (calendar_default (), first, last);
  for (int i = 0; i < g->ndays; i++)
    layout_day (g, occ, i);
  for (guint i = 0; i < g->blocks->len; i++)
    {
      const Block *b = &g_array_index (g->blocks, Block, i);
      const CalEvent *ev = calendar_find (calendar_default (), b->id);
      if (!ev)
        continue;
      gboolean being_dragged = g->drag != DRAG_NONE && g->drag != DRAG_CREATE && g->moved && g->drag_id && g_str_equal (g->drag_id, b->id);
      if (being_dragged)
        {
          /* leave a faint outline where it came from */
          rounded (cr, b->x, b->y, b->w, b->h, 6);
          cairo_set_source_rgba (cr, fg.red, fg.green, fg.blue, 0.12);
          cairo_set_dash (cr, (double[]) { 4, 3 }, 2, 0);
          cairo_stroke (cr);
          cairo_set_dash (cr, NULL, 0, 0);
          continue;
        }
      draw_block (g, cr, b, ev, b->occ_start, b->occ_start + (ev->end - ev->start), FALSE, &fg);
    }

  /* what is being dragged */
  if (g->moved && g->drag != DRAG_NONE && g->ghost_end > g->ghost_start)
    {
      for (int i = 0; i < g->ndays; i++)
        {
          gint64 day = day_at (g, i), next = calendar_day_next (day);
          if (g->ghost_end <= day || g->ghost_start >= next)
            continue;
          gint64 s = MAX (g->ghost_start, day), e = MIN (g->ghost_end, next);
          Block b = { GUTTER + i * cw + 2, minutes_to_y (minutes_in_day (s)), cw - 6,
                      MAX ((double) (e - s) / 3600.0 * HOUR_H - 2, 18), NULL, 0, 0 };
          if (g->drag == DRAG_CREATE)
            {
              CalEvent fake = { 0 };
              g_autofree char *t = g_strdup (TR ("Nuevo evento", "New event"));
              fake.title = t;
              draw_block (g, cr, &b, &fake, s, e, TRUE, &fg);
            }
          else
            {
              const CalEvent *ev = calendar_find (calendar_default (), g->drag_id);
              if (ev)
                {
                  b.color = calendar_calendar_find (calendar_default (), ev->calendar)->color;
                  draw_block (g, cr, &b, ev, s, e, TRUE, &fg);
                }
            }
        }
    }
  g_array_free (occ, TRUE);

  /* the current time */
  for (int i = 0; i < g->ndays; i++)
    {
      gint64 day = day_at (g, i);
      if (now >= day && now < calendar_day_next (day))
        {
          double y = minutes_to_y (minutes_in_day (now));
          cairo_set_source_rgba (cr, 0.88, 0.11, 0.14, 1);
          cairo_set_line_width (cr, 1.5);
          cairo_move_to (cr, GUTTER + i * cw - 4, y);
          cairo_line_to (cr, GUTTER + (i + 1) * cw, y);
          cairo_stroke (cr);
          cairo_arc (cr, GUTTER + i * cw, y, 4, 0, 2 * G_PI);
          cairo_fill (cr);
        }
    }
}

/* ---- picking ------------------------------------------------------------ */

static const Block *
block_at (const Grid *g, double x, double y)
{
  for (guint i = g->blocks->len; i > 0; i--)
    {
      const Block *b = &g_array_index (g->blocks, Block, i - 1);
      if (x >= b->x && x <= b->x + b->w && y >= b->y && y <= b->y + b->h)
        return b;
    }
  return NULL;
}

static void
set_cursor (Grid *g, const char *name)
{
  gtk_widget_set_cursor_from_name (g->area, name);
}

static void
on_motion (GtkEventControllerMotion *c, double x, double y, gpointer data)
{
  (void) c;
  Grid *g = data;
  if (g->drag != DRAG_NONE)
    return;
  const Block *b = block_at (g, x, y);
  if (!b)
    set_cursor (g, NULL);
  else if (y > b->y + b->h - EDGE)
    set_cursor (g, "ns-resize");
  else
    set_cursor (g, "grab");
}

/* ---- dragging ----------------------------------------------------------- */

static void
drag_reset (Grid *g)
{
  g->drag = DRAG_NONE;
  g->moved = FALSE;
  g_clear_pointer (&g->drag_id, g_free);
  set_cursor (g, NULL);
}

static void
on_drag_begin (GtkGestureDrag *gesture, double x, double y, gpointer data)
{
  Grid *g = data;
  drag_reset (g);
  g->press_x = x;
  g->press_y = y;
  if (x < GUTTER)
    return;
  const Block *b = block_at (g, x, y);
  if (b)
    {
      const CalEvent *ev = calendar_find (calendar_default (), b->id);
      if (!ev)
        return;
      g->drag = y > b->y + b->h - EDGE ? DRAG_RESIZE : DRAG_MOVE;
      g->drag_id = g_strdup (b->id);
      g->drag_occ_start = b->occ_start;
      g->drag_start = b->occ_start;
      g->drag_end = b->occ_start + (ev->end - ev->start);
      g->drag_grab_min = y_to_minutes (y - b->y, FALSE);
      g_free (g->selected);
      g->selected = g_strdup (b->id);
      gtk_widget_queue_draw (g->area);
    }
  else
    {
      g->drag = DRAG_CREATE;
      g_clear_pointer (&g->selected, g_free);
    }
  (void) gesture;
}

static void
update_ghost (Grid *g, double x, double y)
{
  if (g->drag == DRAG_CREATE)
    {
      int c0 = x_to_column (g, g->press_x), c1 = x_to_column (g, x);
      int m0 = y_to_minutes (g->press_y, TRUE), m1 = y_to_minutes (y, TRUE);
      /* one column only: a drag always stays on the day where it started */
      (void) c1;
      gint64 day = day_at (g, c0);
      int a = MIN (m0, m1), b = MAX (m0, m1);
      if (b - a < SNAP_MIN)
        b = a + 60;
      g->ghost_start = time_at (day, a);
      g->ghost_end = time_at (day, MIN (b, 24 * 60));
    }
  else if (g->drag == DRAG_MOVE)
    {
      int col = x_to_column (g, x);
      int top = y_to_minutes (y - minutes_to_y (g->drag_grab_min), TRUE);
      gint64 length = g->drag_end - g->drag_start;
      top = CLAMP (top, 0, 24 * 60 - 15);
      g->ghost_start = time_at (day_at (g, col), top);
      g->ghost_end = g->ghost_start + length;
    }
  else if (g->drag == DRAG_RESIZE)
    {
      int start_min = minutes_in_day (g->drag_start);
      int end_min = y_to_minutes (y, TRUE);
      if (end_min < start_min + SNAP_MIN)
        end_min = start_min + SNAP_MIN;
      g->ghost_start = g->drag_start;
      g->ghost_end = g->drag_start + (gint64) (end_min - start_min) * 60;
    }
}

static void
on_drag_update (GtkGestureDrag *gesture, double dx, double dy, gpointer data)
{
  Grid *g = data;
  if (g->drag == DRAG_NONE)
    return;
  if (!g->moved && fabs (dx) < MIN_DRAG && fabs (dy) < MIN_DRAG)
    return;
  g->moved = TRUE;
  set_cursor (g, g->drag == DRAG_RESIZE ? "ns-resize" : g->drag == DRAG_MOVE ? "grabbing" : "crosshair");
  update_ghost (g, g->press_x + dx, g->press_y + dy);
  gtk_widget_queue_draw (g->area);
  (void) gesture;
}

static void
on_drag_end (GtkGestureDrag *gesture, double dx, double dy, gpointer data)
{
  Grid *g = data;
  (void) gesture;
  (void) dx;
  (void) dy;
  if (g->drag != DRAG_NONE && g->moved)
    {
      if (g->drag == DRAG_CREATE)
        {
          if (g->cb.create)
            g->cb.create (g->ghost_start, g->ghost_end, g->data);
        }
      else if (g->drag_id)
        {
          if (g->cb.moved && (g->ghost_start != g->drag_start || g->ghost_end != g->drag_end))
            g->cb.moved (g->drag_id, g->drag_occ_start, g->ghost_start, g->ghost_end, g->data);
        }
    }
  drag_reset (g);
  gtk_widget_queue_draw (g->area);
}

static void
on_click (GtkGestureClick *gesture, int n_press, double x, double y, gpointer data)
{
  Grid *g = data;
  (void) gesture;
  const Block *b = block_at (g, x, y);
  if (!b)
    {
      if (n_press >= 2 && x >= GUTTER)
        {
          gint64 day = day_at (g, x_to_column (g, x));
          int m = y_to_minutes (y, TRUE);
          if (g->cb.create)
            g->cb.create (time_at (day, m), time_at (day, MIN (m + 60, 24 * 60)), g->data);
        }
      else
        {
          g_clear_pointer (&g->selected, g_free);
          gtk_widget_queue_draw (g->area);
        }
      return;
    }
  g_free (g->selected);
  g->selected = g_strdup (b->id);
  gtk_widget_queue_draw (g->area);
  if (n_press >= 2)
    {
      if (g->cb.open)
        g->cb.open (b->id, b->occ_start, g->data);
    }
  else if (g->cb.picked)
    g->cb.picked (b->id, b->occ_start, g->area, b->x + b->w / 2, b->y + b->h / 2, g->data);
}

/* ---- the clock: redraw once a minute, and only while visible ------------ */

static gboolean
on_clock (gpointer data)
{
  Grid *g = data;
  gtk_widget_queue_draw (g->area);
  return G_SOURCE_CONTINUE;
}

static void
on_map (GtkWidget *w, gpointer data)
{
  (void) w;
  Grid *g = data;
  if (!g->clock_id)
    g->clock_id = g_timeout_add_seconds (60, on_clock, g);
}

static void
on_unmap (GtkWidget *w, gpointer data)
{
  (void) w;
  Grid *g = data;
  if (g->clock_id)
    g_source_remove (g->clock_id);
  g->clock_id = 0;
}

static void
grid_free (gpointer data)
{
  Grid *g = data;
  if (g->clock_id)
    g_source_remove (g->clock_id);
  g_array_free (g->blocks, TRUE);
  g_free (g->drag_id);
  g_free (g->selected);
  g_free (g);
}

/* ---- public ------------------------------------------------------------- */

GtkWidget *
cal_grid_new (const CalGridCallbacks *callbacks, gpointer data)
{
  Grid *g = g_new0 (Grid, 1);
  g->cb = *callbacks;
  g->data = data;
  g->ndays = 7;
  g->blocks = g_array_new (FALSE, TRUE, sizeof (Block));
  g_array_set_clear_func (g->blocks, block_clear);

  g->area = gtk_drawing_area_new ();
  gtk_widget_set_hexpand (g->area, TRUE);
  gtk_drawing_area_set_content_height (GTK_DRAWING_AREA (g->area), (int) (24 * HOUR_H));
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (g->area), draw_cb, g, NULL);
  g_object_set_data_full (G_OBJECT (g->area), "grid", g, grid_free);

  GtkGesture *drag = gtk_gesture_drag_new ();
  g_signal_connect (drag, "drag-begin", G_CALLBACK (on_drag_begin), g);
  g_signal_connect (drag, "drag-update", G_CALLBACK (on_drag_update), g);
  g_signal_connect (drag, "drag-end", G_CALLBACK (on_drag_end), g);
  gtk_widget_add_controller (g->area, GTK_EVENT_CONTROLLER (drag));
  GtkGesture *click = gtk_gesture_click_new ();
  g_signal_connect (click, "released", G_CALLBACK (on_click), g);
  gtk_widget_add_controller (g->area, GTK_EVENT_CONTROLLER (click));
  GtkEventController *motion = gtk_event_controller_motion_new ();
  g_signal_connect (motion, "motion", G_CALLBACK (on_motion), g);
  gtk_widget_add_controller (g->area, motion);

  g_signal_connect (g->area, "map", G_CALLBACK (on_map), g);
  g_signal_connect (g->area, "unmap", G_CALLBACK (on_unmap), g);
  return g->area;
}

void
cal_grid_set_days (GtkWidget *grid, gint64 first_day, int ndays)
{
  Grid *g = g_object_get_data (G_OBJECT (grid), "grid");
  g->first_day = first_day;
  g->ndays = ndays;
  gtk_widget_queue_draw (grid);
}

void
cal_grid_refresh (GtkWidget *grid)
{
  gtk_widget_queue_draw (grid);
}

double
cal_grid_hour_y (GtkWidget *grid, int hour)
{
  (void) grid;
  return hour * HOUR_H;
}

gboolean
cal_grid_point (GtkWidget *grid, gint64 t, double *x, double *y)
{
  Grid *g = g_object_get_data (G_OBJECT (grid), "grid");
  for (int i = 0; i < g->ndays; i++)
    {
      gint64 day = day_at (g, i);
      if (t >= day && t < calendar_day_next (day))
        {
          *x = GUTTER + (i + 0.5) * column_width (g);
          *y = minutes_to_y (minutes_in_day (t));
          return TRUE;
        }
    }
  return FALSE;
}

void
cal_grid_test_drag (GtkWidget *grid, double x0, double y0, double x1, double y1)
{
  Grid *g = g_object_get_data (G_OBJECT (grid), "grid");
  on_drag_begin (NULL, x0, y0, g);
  on_drag_update (NULL, x1 - x0, y1 - y0, g);
  on_drag_end (NULL, x1 - x0, y1 - y0, g);
}
