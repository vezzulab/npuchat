#pragma once

#include <glib.h>

/* What the model can do with the calendar. Each function takes the user's own words
 * (a title, "tomorrow 3pm") and returns a short text for the model to read: what was
 * done, or why not. ok, when given, tells whether anything was changed.
 *
 * Nothing here trusts the model's arithmetic: dates are worked out from the words by
 * the same reader that handles quick entry, and an event is found by its title, never
 * by an id the model could invent. Deleting is always undoable. */

char *calendar_tool_agenda (const char *when);
char *calendar_tool_add (const char *text, gboolean *ok);
char *calendar_tool_find (const char *query);
char *calendar_tool_change (const char *event, const char *new_time, const char *new_title, const char *new_location, gboolean *ok);
char *calendar_tool_delete (const char *event, gboolean *ok);
char *calendar_tool_undo (gboolean *ok);
char *calendar_tool_free (const char *minutes, const char *when);
char *calendar_tool_done (const char *event, gboolean *ok);
