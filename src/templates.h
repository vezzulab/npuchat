#pragma once

#include <glib.h>

typedef enum {
  CAT_WELLBEING,
  CAT_WORK,
  CAT_STUDY,
  CAT_LANGUAGES,
  CAT_CREATIVE,
  CAT_TECH,
  CAT_DAILY,
  CAT_COUNT,
} TemplateCategory;

/* A ready-made assistant in the gallery, in both UI languages. */
typedef struct {
  const char      *key;
  TemplateCategory category;
  const char      *emoji;
  const char      *name_es, *name_en;
  const char      *desc_es, *desc_en;
  const char      *instr_es, *instr_en;
} AssistantTemplate;

extern const AssistantTemplate assistant_templates[];
extern const guint             assistant_templates_count;

const char              *template_category_name (TemplateCategory c);
const AssistantTemplate *template_find (const char *key);
