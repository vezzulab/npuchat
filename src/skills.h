#pragma once

#include <gio/gio.h>
#include <json-glib/json-glib.h>

/* Skills are tools the model can call by itself (OpenAI-style function
 * calling, which FastFlowLM supports for models such as qwen3 and qwen3.5):
 * a calculator, the date and time, the laptop's status and Wikipedia.
 *
 * Nothing here runs code the model wrote. The calculator is a small parser
 * and every other skill reads one fixed thing. */

typedef struct {
  const char *id;      /* function name sent to the model and settings key */
  gboolean    context_only; /* not a tool: the app puts the answer in the message instead */
  const char *name_es, *name_en;
  const char *desc_es, *desc_en; /* what it does, shown in Preferences */
  gboolean    internet;          /* sends something over the network */
  gboolean    default_on;
} Skill;

const Skill *skills_list (guint *count);
const Skill *skill_find (const char *id);

/* Appends the "tools" member for the enabled skills to an open request object. */
void skills_build_tools (JsonBuilder *b, gboolean (*enabled) (const char *id, gpointer data), gpointer data);

/* result is owned by the callee; ok is FALSE for errors, whose text is
 * written for the model so it can correct itself or tell the user. */
typedef void (*SkillDone) (char *result, gboolean ok, gpointer data);

void skill_run (const char *id, const char *arguments_json, GCancellable *cancel, SkillDone done, gpointer data);

/* Exposed for tests. */
gboolean skill_calculate (const char *expression, char **result, char **error);
char    *skill_system_status (void);
char    *skill_datetime (void);
/* "[Context: it is now jueves 8 de octubre de 2026, 18:12 (UTC-04:00).]", added to
 * the newest user message. Measured on qwen3.5:9b: letting the model call a date
 * tool failed (it invents the date, or reuses a time seen earlier in the chat),
 * while this note was right every time. */
char    *skill_now_note (void);
/* The standing instruction that makes the model reach for the enabled skills. */
char    *skills_instructions (gboolean (*enabled) (const char *id, gpointer data), gpointer data);
char    *skill_calendar_agenda (const char *when);
char    *skill_calendar_add (const char *text, gboolean *ok);
