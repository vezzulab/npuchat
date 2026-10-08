#include "sysinfo.h"

#include <string.h>
#include <sys/resource.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>

#define GB (1024.0 * 1024.0 * 1024.0)

static char *
read_trimmed (const char *path)
{
  char *contents = NULL;
  if (!g_file_get_contents (path, &contents, NULL, NULL))
    return NULL;
  return g_strstrip (contents);
}

static void
detect_npu (SysInfo *info)
{
  g_autoptr (GDir) dir = g_dir_open ("/sys/class/accel", 0, NULL);
  const char *entry;

  while (dir && (entry = g_dir_read_name (dir)))
    {
      g_autofree char *dev = g_build_filename ("/sys/class/accel", entry, "device", NULL);
      g_autofree char *drv_link = g_build_filename (dev, "driver", NULL);
      g_autofree char *drv_target = g_file_read_link (drv_link, NULL);
      g_autofree char *driver = drv_target ? g_path_get_basename (drv_target) : NULL;

      if (g_strcmp0 (driver, "amdxdna") != 0)
        continue;

      g_autofree char *dev_path = g_build_filename (dev, "device", NULL);
      g_autofree char *fw_path = g_build_filename (dev, "fw_version", NULL);
      g_autofree char *id = read_trimmed (dev_path);

      info->npu_present = TRUE;
      info->npu_firmware = read_trimmed (fw_path);

      if (g_strcmp0 (id, "0x1502") == 0)
        {
          /* Phoenix / Hawk Point: first-generation XDNA, not supported. */
          info->npu_name = g_strdup ("AMD XDNA (Phoenix / Hawk Point)");
          info->npu_supported = FALSE;
        }
      else if (g_strcmp0 (id, "0x17f0") == 0)
        {
          info->npu_name = g_strdup ("AMD XDNA 2 (Strix / Krackan)");
          info->npu_supported = TRUE;
        }
      else
        {
          info->npu_name = g_strdup_printf ("AMD XDNA (%s)", id ? id : "?");
          info->npu_supported = TRUE;
        }
      return;
    }
}

static void
detect_cpu (SysInfo *info)
{
  g_autofree char *cpuinfo = NULL;
  if (!g_file_get_contents ("/proc/cpuinfo", &cpuinfo, NULL, NULL))
    return;

  const char *line = strstr (cpuinfo, "model name");
  const char *colon = line ? strchr (line, ':') : NULL;
  if (!colon)
    return;
  const char *end = strchr (colon, '\n');
  g_autofree char *name = g_strndup (colon + 1, end ? (gsize) (end - colon - 1) : strlen (colon + 1));
  info->cpu = g_strdup (g_strstrip (name));
}

static void
detect_memory (SysInfo *info)
{
  g_autofree char *meminfo = NULL;
  if (!g_file_get_contents ("/proc/meminfo", &meminfo, NULL, NULL))
    return;

  const char *line = strstr (meminfo, "MemTotal:");
  if (line)
    info->ram_total_gb = g_ascii_strtod (line + strlen ("MemTotal:"), NULL) * 1024.0 / GB;

  struct rlimit rl;
  info->memlock_unlimited = getrlimit (RLIMIT_MEMLOCK, &rl) == 0 && rl.rlim_cur == RLIM_INFINITY;
}

static void
detect_disk (SysInfo *info)
{
  struct statvfs st;
  if (statvfs (g_get_home_dir (), &st) == 0)
    info->disk_free_gb = (double) st.f_bavail * st.f_frsize / GB;
}

SysInfo *
sysinfo_get (void)
{
  SysInfo *info = g_new0 (SysInfo, 1);
  struct utsname un;

  detect_npu (info);
  detect_cpu (info);
  detect_memory (info);
  detect_disk (info);
  if (uname (&un) == 0)
    info->kernel = g_strdup (un.release);
  return info;
}

void
sysinfo_free (SysInfo *info)
{
  if (!info)
    return;
  g_free (info->npu_name);
  g_free (info->npu_firmware);
  g_free (info->cpu);
  g_free (info->kernel);
  g_free (info);
}
