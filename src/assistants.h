#pragma once

#include <glib.h>

/* A custom chatbot: a name, an emoji and the instructions sent to the model
 * as its system prompt. */
typedef struct {
  char *id;
  char *name;
  char *emoji;
  char *instructions;
  char *template_key; /* gallery template it came from, or NULL */
  GPtrArray *libs;    /* char* ids of document libraries this assistant always uses */
} Assistant;

Assistant *assistant_new (const char *name, const char *emoji, const char *instructions);
void       assistant_free (Assistant *a);
/* Creates an assistant from a gallery template, in the current UI language. */
Assistant *assistant_new_from_template (const char *key);
gboolean   assistants_has_template (GPtrArray *list, const char *key);

/* Stored in $XDG_DATA_HOME/npu-chat/assistants.json. On first run the list
 * is seeded with a few editable examples in the current UI language. */
GPtrArray *assistants_load (void);
void       assistants_save (GPtrArray *list);
Assistant *assistants_find (GPtrArray *list, const char *id);
