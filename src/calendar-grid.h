#pragma once

#include <gtk/gtk.h>

/* The hour-by-hour grid used by the day and week views. Events are drawn as
 * blocks that can be dragged to move them, stretched from their bottom edge
 * to change the duration, or created by dragging over empty time. What
 * happened is reported through the callbacks, and the owner writes it to the calendar. */

typedef struct {
  /* a click on an event, with the event's block position inside the widget */
  void (*picked) (const char *event_id, gint64 occurrence_start, GtkWidget *grid, double x, double y, gpointer data);
  void (*open) (const char *event_id, gint64 occurrence_start, gpointer data);      /* double click */
  void (*create) (gint64 start, gint64 end, gpointer data);                         /* dragged over free time */
  /* right click: on an event (event_id set) or on free time (event_id NULL, time is the moment clicked) */
  void (*context) (const char *event_id, gint64 occurrence_start, gint64 time, GtkWidget *grid, double x, double y, gpointer data);
  /* the check circle of a reminder was clicked */
  void (*toggled) (const char *event_id, gint64 occurrence_start, gpointer data);
  /* moved or resized: the showing that started at occ_start now runs from new_start to new_end */
  void (*moved) (const char *event_id, gint64 occ_start, gint64 new_start, gint64 new_end, gpointer data);
} CalGridCallbacks;

GtkWidget *cal_grid_new (const CalGridCallbacks *callbacks, gpointer data);
/* first_day is local midnight; ndays is 1 for the day view and 7 for the week */
void       cal_grid_set_days (GtkWidget *grid, gint64 first_day, int ndays);
void       cal_grid_refresh (GtkWidget *grid);
double     cal_grid_hour_y (GtkWidget *grid, int hour);

/* For tests: where a time shows up, and a press-drag-release between two points. */
gboolean   cal_grid_point (GtkWidget *grid, gint64 t, double *x, double *y);
void       cal_grid_test_drag (GtkWidget *grid, double x0, double y0, double x1, double y1);
guint      cal_grid_draw_count (GtkWidget *grid);

/* Hours outside [from, to) are shaded: the working day. */
void       cal_grid_set_hours (GtkWidget *grid, int from, int to);
/* The selected event, or NULL; occurrence_start tells which showing. */
const char *cal_grid_selected (GtkWidget *grid, gint64 *occurrence_start);

/* For tests: a right click at a point. */
void       cal_grid_test_context (GtkWidget *grid, double x, double y);
