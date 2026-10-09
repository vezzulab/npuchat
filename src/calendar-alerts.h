#pragma once

#include <gio/gio.h>

/* Notifications shown before events that have an alert. One timer is armed for
 * the next alert only (never polling), and it re-arms after an alert fires, when
 * events change, and at least once an hour so a suspended laptop catches up. */

void calendar_alerts_start (GApplication *app);
void calendar_alerts_reschedule (void);
void calendar_alerts_stop (void);
