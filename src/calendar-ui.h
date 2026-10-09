#pragma once

#include <gtk/gtk.h>

/* The calendar window: a month grid, the chosen day's agenda and an event editor. */

void calendar_ui_open (GtkWidget *parent);
void calendar_ui_close (void);
/* Redraws after the events changed from elsewhere (for instance, the model added one). */
void calendar_ui_refresh (void);

/* The calendar as a widget, for a window of its own. Call calendar_ui_forget
 * when that widget is destroyed. */
GtkWidget *calendar_ui_view_new (void);
void       calendar_ui_forget (void);

/* Drives the week grid with simulated drags and checks the results; prints PASS or FAIL. */
gboolean calendar_ui_selftest (void);
