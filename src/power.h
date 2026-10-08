#pragma once

#include <glib.h>

typedef void (*PowerCb) (gboolean on_battery, gpointer data);

/* Watches UPower's OnBattery property (no polling); cb runs on changes. */
void     power_init (PowerCb cb, gpointer data);
gboolean power_on_battery (void);
void     power_shutdown (void);
