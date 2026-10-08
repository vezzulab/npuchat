#pragma once

#include <glib.h>

typedef struct {
  gboolean npu_present;
  gboolean npu_supported; /* XDNA2 or newer: what FastFlowLM requires */
  char    *npu_name;
  char    *npu_firmware;
  char    *cpu;
  char    *kernel;
  double   ram_total_gb;
  double   disk_free_gb;
  gboolean memlock_unlimited;
} SysInfo;

SysInfo *sysinfo_get (void);
void     sysinfo_free (SysInfo *info);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (SysInfo, sysinfo_free)
