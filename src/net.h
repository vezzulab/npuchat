#pragma once

#include <libsoup/soup.h>

/* The one HTTP client for the internet: update checks and the Wikipedia
 * skill. Talking to the local flm server uses its own session (flm.c). */
SoupSession *net_session (void);
void         net_shutdown (void);
