#pragma once

#include <glib-object.h>

/* In self-test builds, TRACK() records objects whose lifetime our code
 * manages by hand, so the test can assert none are alive at exit.
 * In normal builds it compiles away. */
#ifdef NPU_CHAT_SELFTEST
gpointer selftest_track (gpointer object, const char *what);
#define TRACK(obj) selftest_track ((obj), G_STRLOC)
#else
#define TRACK(obj) (obj)
#endif
