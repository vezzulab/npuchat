#pragma once

#include <glib.h>

/* A passage retrieved from the user's documents for one question. */
typedef struct {
  char *file;
  int   page; /* 0 when the file has no pages */
  char *text;
} StoreSource;

/* One use of a skill while writing a reply. */
typedef struct {
  char    *name;   /* skill id, e.g. "calculator" */
  char    *args;   /* arguments as the model wrote them (JSON) */
  char    *result;
  gboolean ok;
} StoreTool;

typedef struct {
  char *role;    /* "user" or "assistant" */
  char *content; /* answer text sent back to the model */
  char *think;   /* reasoning shown collapsed, may be NULL */
  char *stats;   /* footer line, may be NULL */
  GPtrArray *sources; /* user messages: StoreSource*; NULL = documents were not searched */
  GPtrArray *tools;   /* assistant messages: StoreTool* used before answering, or NULL */
  char *note;    /* user messages: context line (date and time) sent to the model, never shown */
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
  GPtrArray *libs;            /* char* ids of the document libraries used in this chat */
  gint64     updated; /* unix seconds */
  GPtrArray *msgs;    /* StoreMsg*, only filled while the chat is open */
  gboolean   loaded;
} Conversation;

Conversation *conversation_new (void);
void          conversation_free (Conversation *c);
void          conversation_add (Conversation *c, const char *role, const char *content,
                                const char *think, const char *stats);
void          conversation_set_author (Conversation *c, const char *name, const char *emoji); /* last msg */
void          conversation_set_note (Conversation *c, const char *note); /* last msg */
void          conversation_add_tool (Conversation *c, const char *name, const char *args, const char *result, gboolean ok);
/* Marks the last message as searched (even with no hits) and adds a passage to it. */
void          conversation_begin_sources (Conversation *c);
void          conversation_add_source (Conversation *c, const char *file, int page, const char *text);
gboolean      conversation_has_lib (const Conversation *c, const char *id);
void          conversation_toggle_lib (Conversation *c, const char *id);
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
