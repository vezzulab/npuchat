#include "updater.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <string.h>

#include "net.h"
#include "selftest.h"

#define RELEASES_API "https://api.github.com/repos/vezzulab/npuchat/releases/latest"
#define ASSET_NAME   "NPU-Chat-x86_64.AppImage"

static const char *
api_url (void)
{
  /* Lets the update flow be tested against a local server. */
  const char *env = g_getenv ("NPU_CHAT_UPDATE_URL");
  return env && *env ? env : RELEASES_API;
}

void
update_info_free (UpdateInfo *info)
{
  if (!info)
    return;
  g_free (info->version);
  g_free (info->page_url);
  g_free (info->appimage_url);
  g_free (info->sha256_url);
  g_free (info);
}

/* Compares dotted versions numerically: "0.10.0" > "0.9.2". */
static int
version_compare (const char *a, const char *b)
{
  g_auto (GStrv) pa = g_strsplit (a, ".", -1);
  g_auto (GStrv) pb = g_strsplit (b, ".", -1);
  for (int i = 0; pa[i] || pb[i]; i++)
    {
      long x = pa[i] ? strtol (pa[i], NULL, 10) : 0;
      long y = pb[i] ? strtol (pb[i], NULL, 10) : 0;
      if (x != y)
        return x < y ? -1 : 1;
      if (!pa[i] || !pb[i])
        break;
    }
  return 0;
}

typedef struct {
  UpdateCheckCb cb;
  gpointer      data;
} CheckCtx;

static void
check_done (GObject *src, GAsyncResult *res, gpointer user_data)
{
  CheckCtx *ctx = user_data;
  g_autoptr (GError) error = NULL;
  SoupMessage *msg = soup_session_get_async_result_message (SOUP_SESSION (src), res);
  g_autoptr (GBytes) body = soup_session_send_and_read_finish (SOUP_SESSION (src), res, &error);
  g_autoptr (JsonParser) parser = json_parser_new ();
  UpdateInfo *info = NULL;
  const char *err = NULL;

  if (!body)
    err = error->message;
  else if (soup_message_get_status (msg) != SOUP_STATUS_OK)
    err = soup_message_get_reason_phrase (msg);
  else if (!json_parser_load_from_data (parser, g_bytes_get_data (body, NULL), g_bytes_get_size (body), NULL) ||
           !JSON_NODE_HOLDS_OBJECT (json_parser_get_root (parser)))
    err = "invalid response";

  if (!err)
    {
      JsonObject *o = json_node_get_object (json_parser_get_root (parser));
      const char *tag = json_object_get_string_member_with_default (o, "tag_name", "");
      const char *version = tag[0] == 'v' ? tag + 1 : tag;

      if (*version && version_compare (version, NPU_CHAT_VERSION) > 0)
        {
          info = g_new0 (UpdateInfo, 1);
          info->version = g_strdup (version);
          info->page_url = g_strdup (json_object_get_string_member_with_default (o, "html_url", ""));
          JsonNode *assets = json_object_get_member (o, "assets");
          for (guint i = 0; assets && JSON_NODE_HOLDS_ARRAY (assets) &&
                            i < json_array_get_length (json_node_get_array (assets)); i++)
            {
              JsonObject *a = json_array_get_object_element (json_node_get_array (assets), i);
              const char *name = json_object_get_string_member_with_default (a, "name", "");
              const char *url = json_object_get_string_member_with_default (a, "browser_download_url", NULL);
              if (g_str_equal (name, ASSET_NAME))
                info->appimage_url = g_strdup (url);
              else if (g_str_equal (name, ASSET_NAME ".sha256"))
                info->sha256_url = g_strdup (url);
            }
        }
    }

  if (g_getenv ("NPU_CHAT_DEBUG"))
    g_printerr ("[update] %s\n", err ? err : info ? info->version : "up to date");
  ctx->cb (info, err, ctx->data);
  g_free (ctx);
}

void
updater_check (UpdateCheckCb cb, gpointer data)
{
  CheckCtx *ctx = g_new0 (CheckCtx, 1);
  ctx->cb = cb;
  ctx->data = data;
  g_autoptr (SoupMessage) msg = soup_message_new ("GET", api_url ());
  soup_message_headers_append (soup_message_get_request_headers (msg), "Accept", "application/vnd.github+json");
  soup_session_send_and_read_async (net_session (), msg, G_PRIORITY_LOW, NULL, check_done, ctx);
}

gboolean
updater_can_self_update (void)
{
  const char *appimage = g_getenv ("APPIMAGE");
  if (!appimage || !g_file_test (appimage, G_FILE_TEST_IS_REGULAR))
    return FALSE;
  g_autofree char *dir = g_path_get_dirname (appimage);
  return g_access (dir, W_OK) == 0;
}

/* ---- install ---------------------------------------------------------- */

typedef struct {
  UpdateProgressCb progress;
  UpdateDoneCb     done;
  gpointer         data;
  char            *appimage_url;
  char            *expected_sha;
  char            *target;   /* $APPIMAGE */
  char            *tmp_path; /* $APPIMAGE.new */
  GInputStream    *in;
  GOutputStream   *out;
  GChecksum       *sha;
  goffset          total;
  goffset          received;
} InstallCtx;

static void
install_finish (InstallCtx *ctx, gboolean ok, const char *message)
{
  if (ctx->out)
    g_output_stream_close (ctx->out, NULL, NULL);
  if (!ok && ctx->tmp_path)
    g_unlink (ctx->tmp_path);
  ctx->done (ok, message, ctx->data);

  g_clear_object (&ctx->in);
  g_clear_object (&ctx->out);
  g_checksum_free (ctx->sha);
  g_free (ctx->appimage_url);
  g_free (ctx->expected_sha);
  g_free (ctx->target);
  g_free (ctx->tmp_path);
  g_free (ctx);
}

static void read_chunk (InstallCtx *ctx);

static void
chunk_read (GObject *src, GAsyncResult *res, gpointer user_data)
{
  InstallCtx *ctx = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) bytes = g_input_stream_read_bytes_finish (G_INPUT_STREAM (src), res, &error);

  if (!bytes)
    {
      install_finish (ctx, FALSE, error->message);
      return;
    }

  gsize len = g_bytes_get_size (bytes);
  if (len == 0)
    {
      g_output_stream_close (ctx->out, NULL, NULL);
      g_clear_object (&ctx->out);

      const char *got = g_checksum_get_string (ctx->sha);
      if (g_ascii_strcasecmp (got, ctx->expected_sha) != 0)
        {
          install_finish (ctx, FALSE, "SHA-256 mismatch: the download is corrupt or was tampered with");
          return;
        }
      if (g_chmod (ctx->tmp_path, 0755) != 0 || g_rename (ctx->tmp_path, ctx->target) != 0)
        {
          install_finish (ctx, FALSE, g_strerror (errno));
          return;
        }
      install_finish (ctx, TRUE, NULL);
      return;
    }

  const guint8 *data = g_bytes_get_data (bytes, NULL);
  g_checksum_update (ctx->sha, data, len);
  if (!g_output_stream_write_all (ctx->out, data, len, NULL, NULL, &error))
    {
      install_finish (ctx, FALSE, error->message);
      return;
    }
  ctx->received += len;
  if (ctx->total > 0)
    ctx->progress ((double) ctx->received / ctx->total, ctx->data);
  read_chunk (ctx);
}

static void
read_chunk (InstallCtx *ctx)
{
  g_input_stream_read_bytes_async (ctx->in, 256 * 1024, G_PRIORITY_LOW, NULL, chunk_read, ctx);
}

static void
appimage_opened (GObject *src, GAsyncResult *res, gpointer user_data)
{
  InstallCtx *ctx = user_data;
  g_autoptr (GError) error = NULL;
  SoupMessage *msg = soup_session_get_async_result_message (SOUP_SESSION (src), res);
  ctx->in = soup_session_send_finish (SOUP_SESSION (src), res, &error);

  if (!ctx->in)
    {
      install_finish (ctx, FALSE, error->message);
      return;
    }
  if (soup_message_get_status (msg) != SOUP_STATUS_OK)
    {
      install_finish (ctx, FALSE, soup_message_get_reason_phrase (msg));
      return;
    }
  ctx->total = soup_message_headers_get_content_length (soup_message_get_response_headers (msg));

  g_autoptr (GFile) tmp = g_file_new_for_path (ctx->tmp_path);
  GFileOutputStream *out = g_file_replace (tmp, NULL, FALSE, G_FILE_CREATE_REPLACE_DESTINATION, NULL, &error);
  if (!out)
    {
      install_finish (ctx, FALSE, error->message);
      return;
    }
  ctx->out = G_OUTPUT_STREAM (out);
  read_chunk (ctx);
}

static void
sha_downloaded (GObject *src, GAsyncResult *res, gpointer user_data)
{
  InstallCtx *ctx = user_data;
  g_autoptr (GError) error = NULL;
  SoupMessage *msg = soup_session_get_async_result_message (SOUP_SESSION (src), res);
  g_autoptr (GBytes) body = soup_session_send_and_read_finish (SOUP_SESSION (src), res, &error);

  if (!body || soup_message_get_status (msg) != SOUP_STATUS_OK)
    {
      install_finish (ctx, FALSE, body ? soup_message_get_reason_phrase (msg) : error->message);
      return;
    }

  /* "<64 hex chars>  NPU-Chat-x86_64.AppImage" */
  g_autofree char *text = g_strndup (g_bytes_get_data (body, NULL), g_bytes_get_size (body));
  g_strstrip (text);
  char *space = strpbrk (text, " \t");
  if (space)
    *space = '\0';
  if (strlen (text) != 64)
    {
      install_finish (ctx, FALSE, "invalid checksum file");
      return;
    }
  ctx->expected_sha = g_strdup (text);

  g_autoptr (SoupMessage) next = soup_message_new ("GET", ctx->appimage_url);
  soup_session_send_async (net_session (), next, G_PRIORITY_LOW, NULL, appimage_opened, ctx);
}

void
updater_install (const UpdateInfo *info, UpdateProgressCb progress, UpdateDoneCb done, gpointer data)
{
  if (!updater_can_self_update () || !info->appimage_url || !info->sha256_url)
    {
      done (FALSE, "self-update is not available", data);
      return;
    }

  InstallCtx *ctx = g_new0 (InstallCtx, 1);
  ctx->progress = progress;
  ctx->done = done;
  ctx->data = data;
  ctx->appimage_url = g_strdup (info->appimage_url);
  ctx->target = g_strdup (g_getenv ("APPIMAGE"));
  ctx->tmp_path = g_strconcat (ctx->target, ".new", NULL);
  ctx->sha = g_checksum_new (G_CHECKSUM_SHA256);

  g_autoptr (SoupMessage) msg = soup_message_new ("GET", info->sha256_url);
  soup_session_send_and_read_async (net_session (), msg, G_PRIORITY_LOW, NULL, sha_downloaded, ctx);
}

void
updater_restart_after_exit (void)
{
  const char *appimage = g_getenv ("APPIMAGE");
  if (!appimage)
    return;
  /* Wait a moment so this instance has released its D-Bus name. */
  const char *argv[] = { "/bin/sh", "-c", "sleep 1; exec \"$0\"", appimage, NULL };
  g_spawn_async (NULL, (char **) argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, NULL);
}

void
updater_shutdown (void)
{
  net_shutdown ();
}
