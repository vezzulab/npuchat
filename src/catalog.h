#pragma once

#include "flm.h"
#include "sysinfo.h"

/* What the app needs to know about a model. Uses the metadata reported by
 * `flm list --json`, falling back to estimates from the tag (e.g. "qwen3:8b"). */
typedef struct {
  double   params_b;  /* billions of parameters, 0 if unknown */
  double   size_gb;   /* disk space needed */
  double   ram_gb;    /* memory needed while loaded, including KV cache */
  gboolean chat;      /* multi-turn text chat (not speech, embeddings, single-turn) */
  gboolean vision;
  gboolean reasoning;
} ModelInfo;

typedef enum {
  FIT_OK,
  FIT_NO_NPU,
  FIT_RAM,
  FIT_DISK,
} ModelFit;

void        catalog_describe (const FlmModel *model, ModelInfo *out);
const char *catalog_family (const char *name);
ModelFit    catalog_fit (const ModelInfo *model, const SysInfo *sys);
gboolean    catalog_recommended (const ModelInfo *model, const SysInfo *sys);
double      catalog_max_footprint (const SysInfo *sys);
/* Expected decode speed on this laptop, 0 if unknown. */
double      catalog_tokens_per_second (const ModelInfo *model);
