#include "copilotkey.h"

#include <string.h>

#define RULE_FILE "/etc/udev/hwdb.d/90-npuchat-copilot-key.hwdb"
/* Meta + Shift + F19 as Qt encodes it. */
#define SHORTCUT_KEY ((gint32) (0x10000000 | 0x02000000 | 0x01000042))
#define DESKTOP_ID "io.github.vezzulab.NpuChat.desktop"

gboolean
copilotkey_supported (void)
{
  const char *desktop = g_getenv ("XDG_CURRENT_DESKTOP");
  g_autofree char *pkexec = g_find_program_in_path ("pkexec");
  return desktop && strstr (desktop, "KDE") && pkexec;
}

gboolean
copilotkey_installed (void)
{
  return g_file_test (RULE_FILE, G_FILE_TEST_EXISTS);
}

/* DMI strings as the hwdb sees them: only safe characters, spaces as globs. */
static char *
dmi_value (const char *name)
{
  g_autofree char *path = g_strdup_printf ("/sys/class/dmi/id/%s", name);
  g_autofree char *raw = NULL;
  if (!g_file_get_contents (path, &raw, NULL, NULL))
    return NULL;
  g_strstrip (raw);
  GString *s = g_string_new (NULL);
  for (const char *c = raw; *c; c++)
    {
      if (g_ascii_isalnum (*c) || strchr ("._+-", *c))
        g_string_append_c (s, *c);
      else if (*c == ' ')
        g_string_append_c (s, '*');
    }
  if (!s->len)
    {
      g_string_free (s, TRUE);
      return NULL;
    }
  return g_string_free (s, FALSE);
}

typedef struct {
  gboolean         enable;
  CopilotKeyDone   done;
  gpointer         data;
} Job;

static void
job_finish (Job *job, gboolean ok)
{
  job->done (ok, job->data);
  g_free (job);
}

static void
shortcut_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
  Job *job = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &error);
  if (error)
    g_message ("copilot key: could not set the KDE shortcut: %s", error->message);
  job_finish (job, reply != NULL);
}

static void
set_shortcut (Job *job)
{
  g_autoptr (GError) error = NULL;
  GDBusConnection *bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (!bus)
    {
      job_finish (job, FALSE);
      return;
    }
  GVariantBuilder id, keys;
  g_variant_builder_init (&id, G_VARIANT_TYPE ("as"));
  g_variant_builder_add (&id, "s", DESKTOP_ID);
  g_variant_builder_add (&id, "s", "_launch");
  g_variant_builder_add (&id, "s", "NPU Chat");
  g_variant_builder_add (&id, "s", "NPU Chat");
  g_variant_builder_init (&keys, G_VARIANT_TYPE ("ai"));
  g_variant_builder_add (&keys, "i", job->enable ? SHORTCUT_KEY : 0);
  /* flags 6 = SetPresent | NoAutoloading: take this value as the user's choice */
  g_dbus_connection_call (bus, "org.kde.kglobalaccel", "/kglobalaccel", "org.kde.KGlobalAccel", "setShortcut",
                          g_variant_new ("(asaiu)", &id, &keys, 6u), NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL,
                          shortcut_done, job);
  g_object_unref (bus);
}

static void
rule_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
  Job *job = user_data;
  g_autoptr (GError) error = NULL;
  gboolean ok = g_subprocess_wait_check_finish (G_SUBPROCESS (source), res, &error);
  if (!ok)
    {
      g_message ("copilot key: %s", error->message);
      job_finish (job, FALSE);
      return;
    }
  set_shortcut (job);
}

void
copilotkey_set (gboolean enable, CopilotKeyDone done, gpointer data)
{
  Job *job = g_new0 (Job, 1);
  job->enable = enable;
  job->done = done;
  job->data = data;

  g_autoptr (GError) error = NULL;
  g_autoptr (GSubprocess) proc = NULL;
  const char *refresh = "systemd-hwdb update && udevadm trigger --subsystem-match=input --action=change";

  if (enable)
    {
      g_autofree char *vendor = dmi_value ("sys_vendor");
      g_autofree char *product = dmi_value ("product_name");
      if (!vendor || !product)
        {
          job_finish (job, FALSE);
          return;
        }
      g_autofree char *match = g_strdup_printf ("evdev:atkbd:dmi:bvn*:bvr*:bd*:svn%s:pn%s:*", vendor, product);
      g_autofree char *script = g_strdup_printf ("printf '%%s\\n%%s\\n' \"$1\" ' KEYBOARD_KEY_6e=f19' > %s && %s",
                                                 RULE_FILE, refresh);
      proc = g_subprocess_new (G_SUBPROCESS_FLAGS_STDERR_SILENCE, &error, "pkexec", "sh", "-c", script, "sh", match, NULL);
    }
  else
    {
      g_autofree char *script = g_strdup_printf ("rm -f %s && %s", RULE_FILE, refresh);
      proc = g_subprocess_new (G_SUBPROCESS_FLAGS_STDERR_SILENCE, &error, "pkexec", "sh", "-c", script, NULL);
    }
  if (!proc)
    {
      g_message ("copilot key: %s", error->message);
      job_finish (job, FALSE);
      return;
    }
  g_subprocess_wait_check_async (proc, NULL, rule_done, job);
}
