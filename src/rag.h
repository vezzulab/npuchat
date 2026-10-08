#pragma once

#include <glib.h>

/* Retrieval over the user's documents: text and PDF files are split into
 * chunks and searched with BM25. Embeddings are deliberately not used: the
 * NPU embedding model currently returns vectors without semantic signal, and
 * keyword search plus LLM query expansion measured far better. */

typedef struct {
  guint   id;      /* stable within the library, referenced by chunks */
  char   *path;    /* original location */
  char   *name;    /* file name shown in the UI */
  gint64  mtime;
  guint   chunks;
  gsize   bytes;   /* extracted text size */
} RagDoc;

typedef struct {
  char      *id;
  char      *name;
  gboolean   chat_scoped; /* created by dropping files into a chat */
  guint      next_doc;
  gint64     created;
  GPtrArray *docs;        /* RagDoc* */
  gboolean   busy;        /* an import is running */
} RagLibrary;

typedef struct {
  char  *file;
  int    page; /* 0 when the source has no pages */
  char  *text;
  double score;
} RagHit;

/* ---- libraries on disk ($XDG_DATA_HOME/npu-chat/libraries) ------------- */
GPtrArray  *rag_libraries_load (void);
RagLibrary *rag_library_new (const char *name, gboolean chat_scoped);
RagLibrary *rag_library_find (GPtrArray *libs, const char *id);
void        rag_library_save (RagLibrary *lib);
void        rag_library_delete (RagLibrary *lib); /* removes files, not the struct */
void        rag_library_free (gpointer lib);
void        rag_hit_free (gpointer hit);
void        rag_doc_free (gpointer doc);

gboolean    rag_supported_file (const char *path);
gboolean    rag_pdf_available (void);
guint       rag_library_chunk_count (const RagLibrary *lib);

/* ---- importing ---------------------------------------------------------- */
/* Runs in a worker thread. progress and done are called on the main loop.
 * errors is a NULL-terminated list of "name: reason" strings (or NULL). */
typedef void (*RagProgressCb) (double fraction, const char *file, gpointer data);
typedef void (*RagDoneCb) (guint added, char **errors, gpointer data);

void     rag_library_add_files (RagLibrary *lib, char **paths, RagProgressCb progress,
                                RagDoneCb done, gpointer data);
gboolean rag_library_remove_doc (RagLibrary *lib, guint doc_id);

void     rag_shutdown (void); /* waits briefly for running imports */

/* ---- searching ---------------------------------------------------------- */
typedef struct RagIndex RagIndex;

/* Loads the chunks of one or more libraries into a searchable index. */
RagIndex  *rag_index_new (GPtrArray *libs);
void       rag_index_free (RagIndex *index);
/* Returns RagHit*, best first. Empty when no chunk shares a term with the query. */
GPtrArray *rag_search (RagIndex *index, const char *query, guint k);

/* ---- pieces exposed for tests ------------------------------------------- */
GPtrArray *rag_chunk_text (const char *text, int page); /* char* chunks */
char     **rag_tokenize (const char *text);             /* NULL-terminated */
