#pragma once

#include "calendar.h"

/* iCalendar (.ics), the format iPhone, Google, Outlook and Thunderbird exchange.
 * Reads events, to-dos with their repeats, exceptions and alerts; writes the same. */

/* The events in an .ics text, as an array of CalEvent* (free with calendar_event_free).
 * Times in other time zones are converted to local time; broken or unknown parts are skipped. */
GPtrArray *calendar_ics_parse (const char *text);

/* One calendar of the user's (or all of them when calendar_id is NULL) as an .ics text. */
char *calendar_ics_export (Calendar *cal, const char *calendar_id);
