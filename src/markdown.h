#pragma once

#include <glib.h>

typedef enum {
  MD_TEXT,
  MD_CODE,
} MdKind;

typedef struct {
  MdKind kind;
  char *text; /* Pango markup for MD_TEXT, raw code for MD_CODE */
  char *lang; /* MD_CODE only, may be empty */
} MdBlock;

/* Splits Markdown into text blocks (as Pango markup) and fenced code blocks.
 * Tolerates incomplete input, so it can be called on a streaming reply. */
GPtrArray *md_parse (const char *src);
