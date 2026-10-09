#pragma once

#include <gtk/gtk.h>

typedef enum { PRINT_DAY, PRINT_WEEK, PRINT_MONTH, PRINT_YEAR } PrintView;

/* Prints (or, with export_pdf, writes to a PDF without asking) one page of the calendar:
 * the day, week, month or year that contains `anchor`. */
void calendar_print (GtkWindow *parent, PrintView view, gint64 anchor, const char *export_pdf);
