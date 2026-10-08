#include "catalog.h"

#include <string.h>

/* Decoding reads the whole model from RAM for every token, so speed is
 * bandwidth / model size. Measured: qwen3.5:9b (8.7 GB) at 11.7 tok/s on the
 * Ryzen AI 5 430, i.e. ~100 GB/s of effective memory bandwidth. */
#define EFFECTIVE_BANDWIDTH_GBS 100.0

/* RAM kept free for the desktop and other apps. */
#define SYSTEM_RESERVE_GB 3.5
/* KV cache and runtime buffers on top of estimated weights. */
#define RUNTIME_OVERHEAD_GB 1.5
/* flm's "footprint" already covers the runtime; leave room for the KV cache. */
#define KV_CACHE_GB 1.0

static double
parse_params (const char *name)
{
  const char *tag = strchr (name, ':');
  if (!tag)
    return 0;

  g_autoptr (GRegex) re = g_regex_new ("(?:^|[^a-z0-9.])(e?)(\\d+(?:\\.\\d+)?)b", 0, 0, NULL);
  g_autoptr (GMatchInfo) mi = NULL;
  /* Match from the ':' so it acts as the leading boundary. */
  if (!g_regex_match (re, tag, 0, &mi))
    return 0;

  g_autofree char *effective = g_match_info_fetch (mi, 1);
  g_autofree char *num = g_match_info_fetch (mi, 2);
  double n = g_ascii_strtod (num, NULL);
  /* Gemma 3n "E2B"/"E4B" tags give effective size; real weights are ~2.5x. */
  return *effective ? n * 2.5 : n;
}

static gboolean
has_label (const FlmModel *m, const char *label)
{
  return m->labels && g_strv_contains ((const char *const *) m->labels, label);
}

void
catalog_describe (const FlmModel *m, ModelInfo *out)
{
  const char *name = m->name;
  memset (out, 0, sizeof *out);

  out->params_b = m->params_b > 0 ? m->params_b : parse_params (name);

  if (m->footprint_gb > 0)
    {
      out->size_gb = m->footprint_gb;
      out->ram_gb = m->footprint_gb + KV_CACHE_GB;
    }
  else if (out->params_b > 0)
    {
      out->size_gb = out->params_b * 0.62 + 0.25;
      out->ram_gb = out->size_gb + RUNTIME_OVERHEAD_GB;
    }

  if (m->labels)
    {
      out->chat = !has_label (m, "embeddings") && !has_label (m, "transcription") &&
                  !has_label (m, "single-turn") && !has_label (m, "translation");
      out->vision = has_label (m, "vision");
      out->reasoning = has_label (m, "reasoning");
      return;
    }

  /* No metadata (e.g. an older flm): guess from the tag. */
  out->chat = !strstr (name, "whisper") && !strstr (name, "embed") && !strstr (name, "smolvla");
  out->vision = strstr (name, "vl") || g_str_has_prefix (name, "qwen3.5") ||
                g_str_has_prefix (name, "qwen3.6") || g_str_has_prefix (name, "qwen3.8") ||
                ((g_str_has_prefix (name, "gemma3:") || g_str_has_prefix (name, "medgemma")) &&
                 out->params_b >= 4);
  out->reasoning = (g_str_has_prefix (name, "qwen3") && !g_str_has_prefix (name, "qwen3-it") &&
                    !g_str_has_prefix (name, "qwen3vl")) ||
                   strstr (name, "deepseek") || strstr (name, "-tk") ||
                   g_str_has_prefix (name, "gpt-oss") || strstr (name, "reason");
}

const char *
catalog_family (const char *name)
{
  static const struct {
    const char *prefix;
    const char *family;
  } families[] = {
    { "llama", "Meta Llama" },
    { "qwen", "Alibaba Qwen" },
    { "medgemma", "Google MedGemma" },
    { "translategemma", "Google TranslateGemma" },
    { "embeddinggemma", "Google EmbeddingGemma" },
    { "gemma", "Google Gemma" },
    { "phi", "Microsoft Phi" },
    { "deepseek", "DeepSeek" },
    { "gpt-oss", "OpenAI gpt-oss" },
    { "whisper", "OpenAI Whisper" },
    { "lfm", "Liquid AI LFM" },
    { "nanbeige", "Nanbeige" },
    { "hy-mt", "Tencent Hunyuan MT" },
    { "smolvla", "Hugging Face SmolVLA" },
  };

  for (gsize i = 0; i < G_N_ELEMENTS (families); i++)
    if (g_str_has_prefix (name, families[i].prefix))
      return families[i].family;
  return NULL;
}

ModelFit
catalog_fit (const ModelInfo *model, const SysInfo *sys)
{
  if (!sys->npu_supported)
    return FIT_NO_NPU;
  if (model->ram_gb > 0 && sys->ram_total_gb > 0 &&
      model->ram_gb > sys->ram_total_gb - SYSTEM_RESERVE_GB)
    return FIT_RAM;
  if (model->size_gb > 0 && sys->disk_free_gb > 0 && model->size_gb * 1.1 > sys->disk_free_gb)
    return FIT_DISK;
  return FIT_OK;
}

gboolean
catalog_recommended (const ModelInfo *model)
{
  /* The sweet spot for a 16 GB laptop NPU: 7-9B general chat models. */
  return model->chat && model->params_b >= 7 && model->params_b <= 9.5;
}

double
catalog_max_footprint (const SysInfo *sys)
{
  return MAX (sys->ram_total_gb - SYSTEM_RESERVE_GB - KV_CACHE_GB, 0);
}

double
catalog_tokens_per_second (const ModelInfo *model)
{
  return model->size_gb > 0 ? EFFECTIVE_BANDWIDTH_GBS / model->size_gb : 0;
}
