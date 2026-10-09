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

/* Call before the first view: the app has its own window, so it keeps its own appearance setting. */
void calendar_ui_set_standalone (gboolean on);

/* for screenshots: "editor", "editor-repeat", "settings" or "goto" */
void calendar_ui_debug_open (const char *what);

/* The standalone app keeps running for alerts after its window closes, and can start at
 * login. The settings window asks main to apply a change through this handler. */
void calendar_ui_set_background_handler (void (*handler) (gboolean background, gboolean autostart));

/* for tests: writes day.pdf, week.pdf, month.pdf and year.pdf into the directory */
void calendar_ui_debug_print (const char *directory);

/* for tests: many view and month changes in a row */
void calendar_ui_stress (int rounds);
