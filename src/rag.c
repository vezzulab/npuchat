#include "rag.h"

#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_POPPLER
#include <poppler.h>
#endif

#define CHUNK_TARGET 600   /* characters a chunk grows to before it is closed */
#define CHUNK_MAX    1300  /* hard limit, long paragraphs are split */
#define CHUNK_MIN    24    /* shorter fragments carry no information */
#define OVERLAP      150   /* characters repeated at the start of the next chunk */
#define MAX_TEXT     (24 * 1024 * 1024)
#define RELATIVE_CUTOFF 0.35

/* ---- tokenizer ---------------------------------------------------------- */

/* Lower-cases and strips accents so "garantía" matches "garantia". */
static char *
fold (const char *s)
{
  g_autofree char *nfd = g_utf8_normalize (s, -1, G_NORMALIZE_NFD);
  if (!nfd)
    return g_strdup ("");
  GString *out = g_string_sized_new (strlen (nfd));

  for (const char *p = nfd; *p; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);
      if (g_unichar_type (c) == G_UNICODE_NON_SPACING_MARK)
        continue;
      g_string_append_unichar (out, g_unichar_tolower (c));
    }
  return g_string_free (out, FALSE);
}

static GHashTable *
stopwords (void)
{
  static GHashTable *set;
  if (set)
    return set;
  static const char *words =
    /* Spanish */
    "el la los las un una unos unas de del al y o u e en a para por con sin que se su sus es son lo le les mi tu "
    "me te nos como cuando cuanto cuantos cuantas cuanta donde quien cual cuales cada hasta desde sobre ya no si "
    "mas muy esta este estos estas eso esto ese esa hay ser fue era he ha han hace hago puedo puede pueden "
    /* English */
    "the a an and or of to in on at for with without that this these those is are was were be been it its as by "
    "from how what when where which who why do does did can could should would will my your our their i you we "
    "they he she there here not no yes if then than so very";
  set = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  g_auto (GStrv) list = g_strsplit (words, " ", -1);
  for (int i = 0; list[i]; i++)
    if (*list[i])
      g_hash_table_add (set, g_strdup (list[i]));
  return set;
}

/* Strips common Spanish and English endings. Deliberately light: it only has
 * to make "cancelar" and "cancelación" meet, not be linguistically correct. */
static void
stem (char *w)
{
  static const char *suffixes[] = {
    "aciones", "acion", "mente", "ando", "iendo", "ados", "adas", "idos", "idas", "ado", "ada", "ido", "ida",
    "ing", "ed", "ar", "er", "ir", "es", "os", "as", "s", NULL,
  };
  size_t len = strlen (w);
  for (int i = 0; suffixes[i]; i++)
    {
      size_t sl = strlen (suffixes[i]);
      if (len >= sl + 4 && memcmp (w + len - sl, suffixes[i], sl) == 0)
        {
          w[len - sl] = '\0';
          return;
        }
    }
}

char **
rag_tokenize (const char *text)
{
  g_autofree char *folded = fold (text);
  GPtrArray *out = g_ptr_array_new ();
  GHashTable *stop = stopwords ();
  const char *p = folded;

  while (*p)
    {
      while (*p && !g_unichar_isalnum (g_utf8_get_char (p)))
        p = g_utf8_next_char (p);
      const char *start = p;
      while (*p && g_unichar_isalnum (g_utf8_get_char (p)))
        p = g_utf8_next_char (p);
      if (p == start)
        continue;

      char *word = g_strndup (start, p - start);
      if (strlen (word) < 2 || g_hash_table_contains (stop, word))
        {
          g_free (word);
          continue;
        }
      stem (word);
      g_ptr_array_add (out, word);
    }
  g_ptr_array_add (out, NULL);
  return (char **) g_ptr_array_free (out, FALSE);
}

/* ---- chunking ----------------------------------------------------------- */

static void
emit_chunk (GPtrArray *out, const char *heading, const char *text, gsize len)
{
  g_autofree char *body = g_strndup (text, len);
  g_strstrip (body);
  if (g_utf8_strlen (body, -1) < CHUNK_MIN)
    return;
  /* The section title is the best retrieval hint a short chunk can carry. */
  if (heading && *heading && !g_str_has_prefix (body, heading))
    g_ptr_array_add (out, g_strdup_printf ("%s\n%s", heading, body));
  else
    g_ptr_array_add (out, g_steal_pointer (&body));
}

/* Where to cut a run of text so the cut falls after a sentence or a word. */
static gsize
cut_point (const char *text, gsize len, gsize limit)
{
  if (len <= limit)
    return len;
  gsize best = 0;
  for (gsize i = limit * 6 / 10; i < limit; i++)
    if ((text[i] == '.' || text[i] == '!' || text[i] == '?' || text[i] == '\n') &&
        (text[i + 1] == ' ' || text[i + 1] == '\n' || text[i + 1] == '\0'))
      best = i + 1;
  if (best)
    return best;
  for (gsize i = limit; i > limit / 2; i--)
    if (text[i] == ' ')
      return i;
  /* No space at all: cut on a character boundary. */
  const char *prev = g_utf8_find_prev_char (text, text + limit);
  return prev ? (gsize) (prev - text) : limit;
}

/* Start of the overlap: the last OVERLAP characters, from a word boundary. */
static const char *
overlap_start (const char *text, gsize len)
{
  if (len <= OVERLAP)
    return text;
  const char *p = text + len - OVERLAP;
  while (*p && *p != ' ' && *p != '\n')
    p++;
  while (*p == ' ' || *p == '\n')
    p++;
  return p;
}

GPtrArray *
rag_chunk_text (const char *text, int page)
{
  (void) page;
  GPtrArray *chunks = g_ptr_array_new_with_free_func (g_free);
  g_autofree char *heading = NULL;
  g_autoptr (GString) cur = g_string_new (NULL);
  gboolean has_new_content = FALSE; /* cur holds more than just the overlap */

  g_auto (GStrv) paragraphs = NULL;
  {
    g_autofree char *norm = g_strdup (text);
    /* "\r\n" and form feeds from PDFs become plain newlines. */
    for (char *c = norm; *c; c++)
      if (*c == '\r' || *c == '\f')
        *c = '\n';
    g_autoptr (GRegex) re = g_regex_new ("\n[ \t]*\n+", 0, 0, NULL);
    paragraphs = g_regex_split (re, norm, 0);
  }

  for (int i = 0; paragraphs[i]; i++)
    {
      g_autofree char *para = g_strdup (paragraphs[i]);
      g_strstrip (para);
      if (!*para)
        continue;

      /* A Markdown heading starts a section; it is not a paragraph itself. */
      if (para[0] == '#')
        {
          char *nl = strchr (para, '\n');
          g_autofree char *line = nl ? g_strndup (para, nl - para) : g_strdup (para);
          const char *t = line;
          while (*t == '#')
            t++;
          while (*t == ' ')
            t++;
          if (has_new_content)
            emit_chunk (chunks, heading, cur->str, cur->len);
          g_string_truncate (cur, 0);
          has_new_content = FALSE;
          g_free (heading);
          heading = g_strdup (t);
          if (!nl)
            continue;
          memmove (para, nl + 1, strlen (nl + 1) + 1);
          g_strstrip (para);
          if (!*para)
            continue;
        }

      const char *rest = para;
      gsize rest_len = strlen (para);
      while (rest_len > 0)
        {
          gsize room = cur->len >= CHUNK_TARGET ? 0 : CHUNK_TARGET - cur->len;
          if (rest_len + (cur->len ? 2 : 0) <= room || (cur->len == 0 && rest_len <= CHUNK_MAX))
            {
              if (cur->len)
                g_string_append (cur, "\n\n");
              g_string_append_len (cur, rest, rest_len);
              has_new_content = TRUE;
              break;
            }

          if (cur->len == 0 || !has_new_content)
            {
              /* The paragraph alone is too long: cut it and keep going. */
              gsize cut = cut_point (rest, rest_len, CHUNK_MAX);
              g_string_append_len (cur, rest, cut);
              has_new_content = TRUE;
              rest += cut;
              rest_len -= cut;
              while (rest_len > 0 && (*rest == ' ' || *rest == '\n'))
                {
                  rest++;
                  rest_len--;
                }
            }
          /* Close the current chunk and carry a little context forward. */
          emit_chunk (chunks, heading, cur->str, cur->len);
          g_autofree char *carry = g_strdup (overlap_start (cur->str, cur->len));
          g_string_assign (cur, carry);
          has_new_content = FALSE;
        }
    }

  if (has_new_content)
    emit_chunk (chunks, heading, cur->str, cur->len);
  return chunks;
}

/* ---- file types and text extraction ------------------------------------- */

gboolean
rag_pdf_available (void)
{
#ifdef HAVE_POPPLER
  return TRUE;
#else
  return FALSE;
#endif
}

static gboolean
has_ext (const char *path, const char *const *list)
{
  const char *dot = strrchr (path, '.');
  if (!dot)
    return FALSE;
  for (int i = 0; list[i]; i++)
    if (g_ascii_strcasecmp (dot + 1, list[i]) == 0)
      return TRUE;
  return FALSE;
}

static const char *const text_exts[] = {
  "txt", "md", "markdown", "rst", "org", "csv", "tsv", "json", "yaml", "yml", "toml", "ini", "log",
  "c", "h", "cpp", "hpp", "cc", "py", "js", "ts", "rs", "go", "java", "sh", "html", "xml", "tex", NULL,
};
static const char *const pdf_exts[] = { "pdf", NULL };

gboolean
rag_supported_file (const char *path)
{
  return has_ext (path, text_exts) || (rag_pdf_available () && has_ext (path, pdf_exts));
}

typedef struct {
  int   page;
  char *text;
} Segment;

static void
segment_free (gpointer p)
{
  Segment *s = p;
  g_free (s->text);
  g_free (s);
}

static void
add_segment (GPtrArray *out, int page, char *text)
{
  Segment *s = g_new0 (Segment, 1);
  s->page = page;
  s->text = text;
  g_ptr_array_add (out, s);
}

#ifdef HAVE_POPPLER
/* PDF text comes with a newline at the end of every visual line. Join them,
 * keep blank lines as paragraph breaks and undo "hyphen-newline" splits. */
static char *
normalize_pdf_text (const char *raw)
{
  GString *out = g_string_sized_new (strlen (raw));
  for (const char *p = raw; *p; p++)
    {
      if (*p == '-' && p[1] == '\n' && g_ascii_islower (p[2]))
        {
          p++;
          continue;
        }
      if (*p == '\n')
        {
          if (p[1] == '\n')
            {
              g_string_append (out, "\n\n");
              while (p[1] == '\n')
                p++;
            }
          else
            g_string_append_c (out, ' ');
          continue;
        }
      g_string_append_c (out, *p);
    }
  return g_string_free (out, FALSE);
}

static gboolean
extract_pdf (const char *path, GPtrArray *segments, GError **error)
{
  g_autofree char *uri = g_filename_to_uri (path, NULL, error);
  if (!uri)
    return FALSE;
  /* Older poppler-glib releases have no g_autoptr support, so release by hand. */
  PopplerDocument *doc = poppler_document_new_from_file (uri, NULL, error);
  if (!doc)
    return FALSE;

  gsize total = 0;
  int pages = poppler_document_get_n_pages (doc);
  for (int i = 0; i < pages && total < MAX_TEXT; i++)
    {
      PopplerPage *page = poppler_document_get_page (doc, i);
      if (!page)
        continue;
      char *raw = poppler_page_get_text (page);
      g_object_unref (page);
      if (raw && *raw)
        {
          char *text = normalize_pdf_text (raw);
          total += strlen (text);
          add_segment (segments, i + 1, text);
        }
      g_free (raw);
    }
  g_object_unref (doc);

  if (total < 40)
    {
      g_set_error_literal (error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                           "no text found (a scanned PDF needs OCR first)");
      return FALSE;
    }
  return TRUE;
}
#endif

static gboolean
extract_text (const char *path, GPtrArray *segments, GError **error)
{
#ifdef HAVE_POPPLER
  if (has_ext (path, pdf_exts))
    return extract_pdf (path, segments, error);
#endif
  g_autofree char *contents = NULL;
  gsize len = 0;
  if (!g_file_get_contents (path, &contents, &len, error))
    return FALSE;
  if (len > MAX_TEXT)
    {
      g_set_error_literal (error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "file too large");
      return FALSE;
    }
  if (memchr (contents, '\0', len < 4096 ? len : 4096))
    {
      g_set_error_literal (error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "not a text file");
      return FALSE;
    }
  char *valid = g_utf8_make_valid (contents, len);
  if (!*valid)
    {
      g_free (valid);
      g_set_error_literal (error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "empty file");
      return FALSE;
    }
  add_segment (segments, 0, valid);
  return TRUE;
}

/* ---- storage ------------------------------------------------------------ */

static char *
libraries_dir (void)
{
  return g_build_filename (g_get_user_data_dir (), "npu-chat", "libraries", NULL);
}

static char *
library_dir (const char *id)
{
  g_autofree char *base = libraries_dir ();
  return g_build_filename (base, id, NULL);
}

static char *
chunks_path (const char *id)
{
  g_autofree char *dir = library_dir (id);
  return g_build_filename (dir, "chunks.jsonl", NULL);
}

void
rag_doc_free (gpointer p)
{
  RagDoc *d = p;
  g_free (d->path);
  g_free (d->name);
  g_free (d);
}

void
rag_hit_free (gpointer p)
{
  RagHit *h = p;
  g_free (h->file);
  g_free (h->text);
  g_free (h);
}

void
rag_library_free (gpointer p)
{
  RagLibrary *lib = p;
  if (!lib)
    return;
  g_free (lib->id);
  g_free (lib->name);
  g_ptr_array_unref (lib->docs);
  g_free (lib);
}

RagLibrary *
rag_library_new (const char *name, gboolean chat_scoped)
{
  RagLibrary *lib = g_new0 (RagLibrary, 1);
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  g_autofree char *stamp = g_date_time_format (now, "%Y%m%d%H%M%S");
  lib->id = g_strdup_printf ("%s-%04x", stamp, g_random_int_range (0, 0xffff));
  lib->name = g_strdup (name);
  lib->chat_scoped = chat_scoped;
  lib->created = g_date_time_to_unix (now);
  lib->next_doc = 1;
  lib->docs = g_ptr_array_new_with_free_func (rag_doc_free);
  rag_library_save (lib);
  return lib;
}

RagLibrary *
rag_library_find (GPtrArray *libs, const char *id)
{
  for (guint i = 0; libs && id && i < libs->len; i++)
    if (g_str_equal (((RagLibrary *) libs->pdata[i])->id, id))
      return libs->pdata[i];
  return NULL;
}

guint
rag_library_chunk_count (const RagLibrary *lib)
{
  guint n = 0;
  for (guint i = 0; i < lib->docs->len; i++)
    n += ((RagDoc *) lib->docs->pdata[i])->chunks;
  return n;
}

void
rag_library_save (RagLibrary *lib)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "id");
  json_builder_add_string_value (b, lib->id);
  json_builder_set_member_name (b, "name");
  json_builder_add_string_value (b, lib->name);
  json_builder_set_member_name (b, "chat_scoped");
  json_builder_add_boolean_value (b, lib->chat_scoped);
  json_builder_set_member_name (b, "next_doc");
  json_builder_add_int_value (b, lib->next_doc);
  json_builder_set_member_name (b, "created");
  json_builder_add_int_value (b, lib->created);
  json_builder_set_member_name (b, "docs");
  json_builder_begin_array (b);
  for (guint i = 0; i < lib->docs->len; i++)
    {
      RagDoc *d = lib->docs->pdata[i];
      json_builder_begin_object (b);
      json_builder_set_member_name (b, "id");
      json_builder_add_int_value (b, d->id);
      json_builder_set_member_name (b, "path");
      json_builder_add_string_value (b, d->path);
      json_builder_set_member_name (b, "name");
      json_builder_add_string_value (b, d->name);
      json_builder_set_member_name (b, "mtime");
      json_builder_add_int_value (b, d->mtime);
      json_builder_set_member_name (b, "chunks");
      json_builder_add_int_value (b, d->chunks);
      json_builder_set_member_name (b, "bytes");
      json_builder_add_int_value (b, (gint64) d->bytes);
      json_builder_end_object (b);
    }
  json_builder_end_array (b);
  json_builder_end_object (b);

  g_autoptr (JsonNode) root = json_builder_get_root (b);
  g_autofree char *json = json_to_string (root, TRUE);
  g_autofree char *dir = library_dir (lib->id);
  g_autofree char *path = g_build_filename (dir, "meta.json", NULL);
  g_mkdir_with_parents (dir, 0700);
  g_file_set_contents_full (path, json, -1, G_FILE_SET_CONTENTS_CONSISTENT, 0600, NULL);
}

static RagLibrary *
load_library (const char *dir)
{
  g_autofree char *path = g_build_filename (dir, "meta.json", NULL);
  g_autoptr (JsonParser) parser = json_parser_new ();
  if (!json_parser_load_from_file (parser, path, NULL))
    return NULL;
  JsonNode *root = json_parser_get_root (parser);
  if (!JSON_NODE_HOLDS_OBJECT (root))
    return NULL;
  JsonObject *o = json_node_get_object (root);
  const char *id = json_object_get_string_member_with_default (o, "id", NULL);
  if (!id)
    return NULL;

  RagLibrary *lib = g_new0 (RagLibrary, 1);
  lib->id = g_strdup (id);
  lib->name = g_strdup (json_object_get_string_member_with_default (o, "name", id));
  lib->chat_scoped = json_object_get_boolean_member_with_default (o, "chat_scoped", FALSE);
  lib->next_doc = (guint) json_object_get_int_member_with_default (o, "next_doc", 1);
  lib->created = json_object_get_int_member_with_default (o, "created", 0);
  lib->docs = g_ptr_array_new_with_free_func (rag_doc_free);

  JsonNode *docs = json_object_get_member (o, "docs");
  for (guint i = 0; docs && JSON_NODE_HOLDS_ARRAY (docs) && i < json_array_get_length (json_node_get_array (docs)); i++)
    {
      JsonObject *d = json_array_get_object_element (json_node_get_array (docs), i);
      if (!d)
        continue;
      RagDoc *doc = g_new0 (RagDoc, 1);
      doc->id = (guint) json_object_get_int_member_with_default (d, "id", 0);
      doc->path = g_strdup (json_object_get_string_member_with_default (d, "path", ""));
      doc->name = g_strdup (json_object_get_string_member_with_default (d, "name", ""));
      doc->mtime = json_object_get_int_member_with_default (d, "mtime", 0);
      doc->chunks = (guint) json_object_get_int_member_with_default (d, "chunks", 0);
      doc->bytes = (gsize) json_object_get_int_member_with_default (d, "bytes", 0);
      g_ptr_array_add (lib->docs, doc);
      if (doc->id >= lib->next_doc)
        lib->next_doc = doc->id + 1;
    }
  return lib;
}

static int
by_created (gconstpointer a, gconstpointer b)
{
  const RagLibrary *la = *(RagLibrary **) a;
  const RagLibrary *lb = *(RagLibrary **) b;
  return (la->created > lb->created) - (la->created < lb->created);
}

GPtrArray *
rag_libraries_load (void)
{
  GPtrArray *libs = g_ptr_array_new_with_free_func (rag_library_free);
  g_autofree char *base = libraries_dir ();
  g_autoptr (GDir) d = g_dir_open (base, 0, NULL);
  const char *name;

  while (d && (name = g_dir_read_name (d)))
    {
      g_autofree char *dir = g_build_filename (base, name, NULL);
      RagLibrary *lib = load_library (dir);
      if (lib)
        g_ptr_array_add (libs, lib);
    }
  g_ptr_array_sort (libs, by_created);
  return libs;
}

void
rag_library_delete (RagLibrary *lib)
{
  g_autofree char *dir = library_dir (lib->id);
  g_autoptr (GDir) d = g_dir_open (dir, 0, NULL);
  const char *name;
  while (d && (name = g_dir_read_name (d)))
    {
      g_autofree char *file = g_build_filename (dir, name, NULL);
      g_unlink (file);
    }
  g_rmdir (dir);
}

/* ---- chunk files -------------------------------------------------------- */

static char *
json_quote (const char *text)
{
  g_autoptr (JsonNode) node = json_node_new (JSON_NODE_VALUE);
  json_node_set_string (node, text);
  g_autoptr (JsonGenerator) gen = json_generator_new ();
  json_generator_set_root (gen, node);
  return json_generator_to_data (gen, NULL);
}

static gboolean
append_chunk (FILE *f, guint doc, int page, const char *text)
{
  g_autofree char *quoted = json_quote (text);
  return fprintf (f, "{\"d\":%u,\"p\":%d,\"t\":%s}\n", doc, page, quoted) > 0;
}

gboolean
rag_library_remove_doc (RagLibrary *lib, guint doc_id)
{
  guint idx = G_MAXUINT;
  for (guint i = 0; i < lib->docs->len; i++)
    if (((RagDoc *) lib->docs->pdata[i])->id == doc_id)
      idx = i;
  if (idx == G_MAXUINT)
    return FALSE;

  g_autofree char *path = chunks_path (lib->id);
  g_autofree char *tmp = g_strconcat (path, ".tmp", NULL);
  g_autofree char *prefix = g_strdup_printf ("{\"d\":%u,", doc_id);
  FILE *in = g_fopen (path, "rb");
  FILE *out = in ? g_fopen (tmp, "wb") : NULL;

  if (in && out)
    {
      size_t cap = 0;
      ssize_t n;
      gsize plen = strlen (prefix);
      char *buf = NULL;
      while ((n = getline (&buf, &cap, in)) > 0)
        if (strncmp (buf, prefix, plen) != 0)
          fwrite (buf, 1, n, out);
      free (buf);
    }
  if (in)
    fclose (in);
  if (out)
    {
      fclose (out);
      g_rename (tmp, path);
    }
  g_ptr_array_remove_index (lib->docs, idx);
  rag_library_save (lib);
  return TRUE;
}

/* ---- importing ---------------------------------------------------------- */

typedef struct {
  RagLibrary   *lib;
  char         *lib_id;
  char        **paths;
  guint         first_id;
  RagProgressCb progress;
  RagDoneCb     done;
  gpointer      data;
  GPtrArray    *new_docs; /* RagDoc*, filled by the worker */
  GPtrArray    *errors;   /* char* */
} ImportCtx;

typedef struct {
  RagProgressCb cb;
  gpointer      data;
  double        fraction;
  char         *file;
} ProgressMsg;

static int running_imports;

static gboolean
progress_idle (gpointer user_data)
{
  ProgressMsg *m = user_data;
  if (m->cb)
    m->cb (m->fraction, m->file, m->data);
  g_free (m->file);
  g_free (m);
  return G_SOURCE_REMOVE;
}

static void
post_progress (ImportCtx *ctx, double fraction, const char *file)
{
  ProgressMsg *m = g_new0 (ProgressMsg, 1);
  m->cb = ctx->progress;
  m->data = ctx->data;
  m->fraction = fraction;
  m->file = g_strdup (file);
  g_main_context_invoke (NULL, progress_idle, m);
}

static void
import_worker (GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable)
{
  (void) source;
  (void) cancellable;
  ImportCtx *ctx = task_data;
  g_autofree char *path = chunks_path (ctx->lib_id);
  guint n_paths = g_strv_length (ctx->paths);

  for (guint i = 0; i < n_paths; i++)
    {
      const char *file = ctx->paths[i];
      g_autofree char *name = g_path_get_basename (file);
      post_progress (ctx, (double) i / n_paths, name);

      g_autoptr (GError) error = NULL;
      g_autoptr (GPtrArray) segments = g_ptr_array_new_with_free_func (segment_free);
      if (!rag_supported_file (file))
        {
          g_ptr_array_add (ctx->errors, g_strdup_printf ("%s: unsupported file type", name));
          continue;
        }
      if (!extract_text (file, segments, &error))
        {
          g_ptr_array_add (ctx->errors, g_strdup_printf ("%s: %s", name, error->message));
          continue;
        }

      FILE *f = g_fopen (path, "ab");
      if (!f)
        {
          g_ptr_array_add (ctx->errors, g_strdup_printf ("%s: %s", name, g_strerror (errno)));
          continue;
        }

      guint doc_id = ctx->first_id + i;
      guint count = 0;
      gsize bytes = 0;
      for (guint s = 0; s < segments->len; s++)
        {
          Segment *seg = segments->pdata[s];
          g_autoptr (GPtrArray) chunks = rag_chunk_text (seg->text, seg->page);
          bytes += strlen (seg->text);
          for (guint c = 0; c < chunks->len; c++)
            {
              append_chunk (f, doc_id, seg->page, chunks->pdata[c]);
              count++;
            }
        }
      fclose (f);

      if (count == 0)
        {
          g_ptr_array_add (ctx->errors, g_strdup_printf ("%s: no usable text", name));
          continue;
        }

      GStatBuf st;
      RagDoc *doc = g_new0 (RagDoc, 1);
      doc->id = doc_id;
      doc->path = g_strdup (file);
      doc->name = g_strdup (name);
      doc->mtime = g_stat (file, &st) == 0 ? (gint64) st.st_mtime : 0;
      doc->chunks = count;
      doc->bytes = bytes;
      g_ptr_array_add (ctx->new_docs, doc);
    }
  post_progress (ctx, 1.0, NULL);
  g_task_return_boolean (task, TRUE);
}

static void
import_finished (GObject *source, GAsyncResult *res, gpointer user_data)
{
  (void) source;
  (void) res;
  ImportCtx *ctx = user_data;
  RagLibrary *lib = ctx->lib;

  guint added = ctx->new_docs->len;
  for (guint i = 0; i < ctx->new_docs->len; i++)
    g_ptr_array_add (lib->docs, ctx->new_docs->pdata[i]);
  g_ptr_array_set_size (ctx->new_docs, 0); /* ownership moved to the library */
  lib->busy = FALSE;
  rag_library_save (lib);

  g_ptr_array_add (ctx->errors, NULL);
  ctx->done (added, (char **) ctx->errors->pdata, ctx->data);

  running_imports--;
  g_ptr_array_unref (ctx->new_docs);
  g_ptr_array_set_free_func (ctx->errors, g_free);
  g_ptr_array_unref (ctx->errors);
  g_free (ctx->lib_id);
  g_strfreev (ctx->paths);
  g_free (ctx);
}

void
rag_library_add_files (RagLibrary *lib, char **paths, RagProgressCb progress, RagDoneCb done, gpointer data)
{
  /* Adding a file again replaces the old copy. */
  for (guint i = 0; paths[i]; i++)
    for (guint d = 0; d < lib->docs->len; d++)
      if (g_str_equal (((RagDoc *) lib->docs->pdata[d])->path, paths[i]))
        {
          rag_library_remove_doc (lib, ((RagDoc *) lib->docs->pdata[d])->id);
          break;
        }

  ImportCtx *ctx = g_new0 (ImportCtx, 1);
  ctx->lib = lib;
  ctx->lib_id = g_strdup (lib->id);
  ctx->paths = g_strdupv (paths);
  ctx->progress = progress;
  ctx->done = done;
  ctx->data = data;
  ctx->new_docs = g_ptr_array_new ();
  ctx->errors = g_ptr_array_new ();

  /* Reserve the ids up front so an interrupted import can never collide
   * with later ones. */
  ctx->first_id = lib->next_doc;
  lib->next_doc += g_strv_length (paths);
  lib->busy = TRUE;
  rag_library_save (lib);

  running_imports++;
  GTask *task = g_task_new (NULL, NULL, import_finished, ctx);
  g_task_set_task_data (task, ctx, NULL);
  g_task_run_in_thread (task, import_worker);
  g_object_unref (task);
}

/* ---- index and search --------------------------------------------------- */

typedef struct {
  guint   lib;
  guint   doc;
  gint    page;
  gsize   offset;
  gsize   length;
  guint   tokens;
} Chunk;

typedef struct {
  guint chunk;
  guint tf;
} Posting;

struct RagIndex {
  GPtrArray  *maps;      /* GMappedFile* per library, NULL when empty */
  GPtrArray  *doc_names; /* per library: GHashTable doc id -> file name */
  GArray     *chunks;    /* Chunk */
  GHashTable *postings;  /* term -> GArray of Posting */
  double      avg_tokens;
};

static void
postings_free (gpointer p)
{
  g_array_unref (p);
}

static void
unmap (gpointer p)
{
  if (p)
    g_mapped_file_unref (p);
}

RagIndex *
rag_index_new (GPtrArray *libs)
{
  RagIndex *ix = g_new0 (RagIndex, 1);
  ix->maps = g_ptr_array_new_with_free_func (unmap);
  ix->doc_names = g_ptr_array_new_with_free_func ((GDestroyNotify) g_hash_table_unref);
  ix->chunks = g_array_new (FALSE, FALSE, sizeof (Chunk));
  ix->postings = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, postings_free);
  gsize total_tokens = 0;

  for (guint l = 0; libs && l < libs->len; l++)
    {
      RagLibrary *lib = libs->pdata[l];
      GHashTable *names = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_free);
      for (guint d = 0; d < lib->docs->len; d++)
        {
          RagDoc *doc = lib->docs->pdata[d];
          g_hash_table_insert (names, GUINT_TO_POINTER (doc->id), g_strdup (doc->name));
        }
      g_ptr_array_add (ix->doc_names, names);

      g_autofree char *path = chunks_path (lib->id);
      GMappedFile *map = g_mapped_file_new (path, FALSE, NULL);
      /* An empty file cannot be mapped. */
      if (map && g_mapped_file_get_length (map) == 0)
        {
          g_mapped_file_unref (map);
          map = NULL;
        }
      g_ptr_array_add (ix->maps, map);
      if (!map)
        continue;

      const char *base = g_mapped_file_get_contents (map);
      gsize size = g_mapped_file_get_length (map);
      gsize pos = 0;
      while (pos < size)
        {
          const char *nl = memchr (base + pos, '\n', size - pos);
          gsize len = nl ? (gsize) (nl - (base + pos)) : size - pos;
          if (len > 8 && strncmp (base + pos, "{\"d\":", 5) == 0)
            {
              guint doc_id = (guint) strtoul (base + pos + 5, NULL, 10);
              if (g_hash_table_contains (names, GUINT_TO_POINTER (doc_id)))
                {
                  g_autoptr (JsonParser) parser = json_parser_new ();
                  if (json_parser_load_from_data (parser, base + pos, len, NULL))
                    {
                      JsonObject *o = json_node_get_object (json_parser_get_root (parser));
                      const char *text = json_object_get_string_member_with_default (o, "t", "");
                      g_auto (GStrv) tokens = rag_tokenize (text);
                      guint chunk_id = ix->chunks->len;
                      Chunk c = {
                        .lib = l, .doc = doc_id, .page = (gint) json_object_get_int_member_with_default (o, "p", 0),
                        .offset = pos, .length = len, .tokens = g_strv_length (tokens),
                      };
                      g_array_append_val (ix->chunks, c);
                      total_tokens += c.tokens;

                      g_autoptr (GHashTable) tf = g_hash_table_new (g_str_hash, g_str_equal);
                      for (int t = 0; tokens[t]; t++)
                        g_hash_table_insert (tf, tokens[t],
                                             GUINT_TO_POINTER (GPOINTER_TO_UINT (g_hash_table_lookup (tf, tokens[t])) + 1));
                      GHashTableIter it;
                      gpointer key, val;
                      g_hash_table_iter_init (&it, tf);
                      while (g_hash_table_iter_next (&it, &key, &val))
                        {
                          GArray *list = g_hash_table_lookup (ix->postings, key);
                          if (!list)
                            {
                              list = g_array_new (FALSE, FALSE, sizeof (Posting));
                              g_hash_table_insert (ix->postings, g_strdup (key), list);
                            }
                          Posting p = { chunk_id, GPOINTER_TO_UINT (val) };
                          g_array_append_val (list, p);
                        }
                    }
                }
            }
          pos += len + 1;
        }
    }
  ix->avg_tokens = ix->chunks->len ? (double) total_tokens / ix->chunks->len : 1.0;
  return ix;
}

void
rag_index_free (RagIndex *ix)
{
  if (!ix)
    return;
  g_ptr_array_unref (ix->maps);
  g_ptr_array_unref (ix->doc_names);
  g_array_unref (ix->chunks);
  g_hash_table_unref (ix->postings);
  g_free (ix);
}

static RagHit *
load_hit (RagIndex *ix, const Chunk *c, double score)
{
  GMappedFile *map = ix->maps->pdata[c->lib];
  g_autoptr (JsonParser) parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, g_mapped_file_get_contents (map) + c->offset, c->length, NULL))
    return NULL;
  JsonObject *o = json_node_get_object (json_parser_get_root (parser));
  GHashTable *names = ix->doc_names->pdata[c->lib];

  RagHit *h = g_new0 (RagHit, 1);
  h->file = g_strdup (g_hash_table_lookup (names, GUINT_TO_POINTER (c->doc)));
  h->page = c->page;
  h->text = g_strdup (json_object_get_string_member_with_default (o, "t", ""));
  h->score = score;
  return h;
}

GPtrArray *
rag_search (RagIndex *ix, const char *query, guint k)
{
  GPtrArray *hits = g_ptr_array_new_with_free_func (rag_hit_free);
  guint n = ix->chunks->len;
  if (n == 0 || k == 0)
    return hits;

  const double k1 = 1.2, b = 0.75;
  g_autofree double *scores = g_new0 (double, n);
  g_auto (GStrv) terms = rag_tokenize (query);
  g_autoptr (GHashTable) seen = g_hash_table_new (g_str_hash, g_str_equal);

  for (int t = 0; terms[t]; t++)
    {
      if (!g_hash_table_add (seen, terms[t]))
        continue;
      GArray *list = g_hash_table_lookup (ix->postings, terms[t]);
      if (!list)
        continue;
      double idf = log (1.0 + (n - list->len + 0.5) / (list->len + 0.5));
      for (guint i = 0; i < list->len; i++)
        {
          Posting *p = &g_array_index (list, Posting, i);
          const Chunk *c = &g_array_index (ix->chunks, Chunk, p->chunk);
          double norm = k1 * (1.0 - b + b * c->tokens / ix->avg_tokens);
          scores[p->chunk] += idf * p->tf * (k1 + 1.0) / (p->tf + norm);
        }
    }

  /* Keep only the best k chunks: k is tiny, so insertion into a short list. */
  guint top[16];
  guint count = 0;
  if (k > G_N_ELEMENTS (top))
    k = G_N_ELEMENTS (top);
  for (guint i = 0; i < n; i++)
    {
      if (scores[i] <= 0 || (count == k && scores[i] <= scores[top[count - 1]]))
        continue;
      guint pos = count < k ? count++ : k - 1;
      while (pos > 0 && scores[top[pos - 1]] < scores[i])
        {
          top[pos] = top[pos - 1];
          pos--;
        }
      top[pos] = i;
    }
  if (count == 0)
    return hits;

  double best = scores[top[0]];
  for (guint i = 0; i < count; i++)
    {
      if (scores[top[i]] < best * RELATIVE_CUTOFF)
        break;
      RagHit *h = load_hit (ix, &g_array_index (ix->chunks, Chunk, top[i]), scores[top[i]]);
      if (h)
        g_ptr_array_add (hits, h);
    }
  return hits;
}

void
rag_shutdown (void)
{
  /* Let a running import finish so no half-written library is left behind. */
  gint64 deadline = g_get_monotonic_time () + 3 * G_USEC_PER_SEC;
  while (running_imports > 0 && g_get_monotonic_time () < deadline)
    if (!g_main_context_iteration (NULL, FALSE))
      g_usleep (10000);
}
