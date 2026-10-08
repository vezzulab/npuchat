#pragma once

#include <glib.h>

typedef enum {
  LANG_ES,
  LANG_EN,
} Lang;

/* pref is "es", "en" or "auto" (follow the system locale). */
void i18n_set (const char *pref);
Lang i18n_lang (void);

/* Every user-visible string is written in both languages at the call site. */
#define TR(es, en) (i18n_lang () == LANG_EN ? (en) : (es))
