#include "i18n.h"

static Lang lang = LANG_ES;

void
i18n_set (const char *pref)
{
  if (g_strcmp0 (pref, "en") == 0)
    lang = LANG_EN;
  else if (g_strcmp0 (pref, "es") == 0)
    lang = LANG_ES;
  else
    lang = g_str_has_prefix (g_get_language_names ()[0], "es") ? LANG_ES : LANG_EN;
}

Lang
i18n_lang (void)
{
  return lang;
}
