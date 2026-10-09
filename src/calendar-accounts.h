#pragma once

#include <gtk/gtk.h>

/* The window for the CalDAV accounts (iCloud for iPhones, Nextcloud…): add one, choose
 * which of its calendars to keep in step, see how the last sync went, remove it. */
void calendar_accounts_open (GtkWidget *parent);
