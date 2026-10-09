#pragma once

#include <glib.h>

/* Calendars fed from a web address: a holiday calendar, a team's shared calendar, a
 * public .ics link. They are read-only and refreshed on a schedule. Nothing is fetched
 * unless the user subscribed, and then only that address. */

typedef struct {
  const char *name_es, *name_en;
  const char *code;      /* Google's public holiday calendar id */
} HolidayRegion;

const HolidayRegion *calendar_holiday_regions (guint *count);
/* The address of a region's holiday calendar. Free with g_free. */
char *calendar_holiday_url (const HolidayRegion *region);

/* "webcal://" becomes "https://"; only http and https are accepted. NULL when it is not an address. */
char *calendar_subscription_normalize_url (const char *text);

typedef void (*SubscribeDone) (const char *calendar_id, guint events, const char *error, gpointer data);

/* Fetches one subscription now and replaces its events. On failure the old events stay.
 * error is NULL on success. done is always called, on the main loop. */
void calendar_subscription_fetch (const char *calendar_id, SubscribeDone done, gpointer data);

/* Refreshes the subscriptions that are due: once shortly after start, then every hour
 * (one timer). changed is called after a refresh changed something. */
void calendar_subscriptions_start (void (*changed) (void));
void calendar_subscriptions_stop (void);
/* Every subscription now, whatever its schedule. */
void calendar_subscriptions_refresh_all (void);
