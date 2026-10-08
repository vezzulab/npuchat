#pragma once

#include <glib.h>

typedef struct {
  char *role;    /* "user" or "assistant" */
  char *content; /* answer text sent back to the model */
  char *think;   /* reasoning shown collapsed, may be NULL */
  char *stats;   /* footer line, may be NULL */
  char *author;  /* team chats: assistant that wrote it, may be NULL */
  char *author_emoji;
} StoreMsg;

/* A team member, snapshotted into the conversation like `system`. */
typedef struct {
  char *id;
  char *name;
  char *emoji;
  char *instructions;
} TeamMember;

typedef struct {
  char      *id;
  char      *title;
  char      *model;
  char      *assistant_id;    /* NULL for the general assistant */
  char      *assistant_name;
  char      *assistant_emoji;
  char      *system;          /* instructions snapshot, so edits don't change old chats */
  GPtrArray *team;            /* TeamMember*, two or more for a team chat, else empty */
  gint64     updated; /* unix seconds */
  GPtrArray *msgs;    /* StoreMsg*, only filled while the chat is open */
  gboolean   loaded;
} Conversation;

Conversation *conversation_new (void);
void          conversation_free (Conversation *c);
void          conversation_add (Conversation *c, const char *role, const char *content,
                                const char *think, const char *stats);
void          conversation_set_author (Conversation *c, const char *name, const char *emoji); /* last msg */
TeamMember   *team_member_new (const char *id, const char *name, const char *emoji, const char *instructions);
void          team_member_free (gpointer member);

/* Conversations live as JSON files in $XDG_DATA_HOME/npu-chat/chats.
 * Listing reads only the headers; messages are loaded on demand so closed
 * chats do not stay in memory. */
GPtrArray *store_load_all (void); /* newest first, messages not loaded */
void       store_load_messages (Conversation *c);
void       store_unload_messages (Conversation *c);
void       store_save (Conversation *c);
void       store_delete (Conversation *c);
