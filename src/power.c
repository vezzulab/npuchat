#include "power.h"

#include <gio/gio.h>

static GDBusProxy *proxy;
static gboolean    on_battery;
static PowerCb     power_cb;
static gpointer    power_data;

/* Fallback when UPower is unavailable: any online "Mains" supply means AC. */
static gboolean
sysfs_on_battery (void)
{
  g_autoptr (GDir) dir = g_dir_open ("/sys/class/power_supply", 0, NULL);
  const char *name;
  gboolean has_mains = FALSE;

  while (dir && (name = g_dir_read_name (dir)))
    {
      g_autofree char *type_path = g_build_filename ("/sys/class/power_supply", name, "type", NULL);
      g_autofree char *online_path = g_build_filename ("/sys/class/power_supply", name, "online", NULL);
      g_autofree char *type = NULL;
      g_autofree char *online = NULL;

      if (!g_file_get_contents (type_path, &type, NULL, NULL) || !g_str_has_prefix (type, "Mains"))
        continue;
      has_mains = TRUE;
      if (g_file_get_contents (online_path, &online, NULL, NULL) && online[0] == '1')
        return FALSE;
    }
  return has_mains;
}

static void
read_property (void)
{
  g_autoptr (GVariant) v = g_dbus_proxy_get_cached_property (proxy, "OnBattery");
  gboolean value = v ? g_variant_get_boolean (v) : sysfs_on_battery ();

  if (value != on_battery)
    {
      on_battery = value;
      if (power_cb)
        power_cb (on_battery, power_data);
    }
}

static void
on_properties_changed (GDBusProxy *p, GVariant *changed, GStrv invalidated, gpointer user_data)
{
  (void) p;
  (void) invalidated;
  (void) user_data;
  g_autoptr (GVariant) v = g_variant_lookup_value (changed, "OnBattery", G_VARIANT_TYPE_BOOLEAN);
  if (v)
    read_property ();
}

static void
on_proxy_ready (GObject *src, GAsyncResult *res, gpointer user_data)
{
  (void) src;
  (void) user_data;
  proxy = g_dbus_proxy_new_for_bus_finish (res, NULL);
  if (!proxy)
    return;
  g_signal_connect (proxy, "g-properties-changed", G_CALLBACK (on_properties_changed), NULL);
  read_property ();
}

void
power_init (PowerCb cb, gpointer data)
{
  power_cb = cb;
  power_data = data;
  on_battery = sysfs_on_battery ();
  g_dbus_proxy_new_for_bus (G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START, NULL,
                            "org.freedesktop.UPower", "/org/freedesktop/UPower",
                            "org.freedesktop.UPower", NULL, on_proxy_ready, NULL);
}

gboolean
power_on_battery (void)
{
  return on_battery;
}

void
power_shutdown (void)
{
  power_cb = NULL;
  g_clear_object (&proxy);
}
