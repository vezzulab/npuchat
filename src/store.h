#pragma once

#include <glib.h>

typedef struct {
  char *role;    /* "user" or "assistant" */
  char *content; /* answer text sent back to the model */
  char *think;   /* reasoning shown collapsed, may be NULL */
  char *stats;   /* footer line, may be NULL */
} StoreMsg;

typedef struct {
  char      *id;
  char      *title;
  char      *model;
  char      *assistant_id;    /* NULL for the general assistant */
  char      *assistant_name;
  char      *assistant_emoji;
  char      *system;          /* instructions snapshot, so edits don't change old chats */
  gint64     updated; /* unix seconds */
  GPtrArray *msgs;    /* StoreMsg*, only filled while the chat is open */
  gboolean   loaded;
} Conversation;

Conversation *conversation_new (void);
void          conversation_free (Conversation *c);
void          conversation_add (Conversation *c, const char *role, const char *content,
                                const char *think, const char *stats);

/* Conversations live as JSON files in $XDG_DATA_HOME/npu-chat/chats.
 * Listing reads only the headers; messages are loaded on demand so closed
 * chats do not stay in memory. */
GPtrArray *store_load_all (void); /* newest first, messages not loaded */
void       store_load_messages (Conversation *c);
void       store_unload_messages (Conversation *c);
void       store_save (Conversation *c);
void       store_delete (Conversation *c);
