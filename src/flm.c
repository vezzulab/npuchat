#include "flm.h"
#include "selftest.h"

#include <json-glib/json-glib.h>
#include <signal.h>
#include <string.h>
#include <sys/prctl.h>

#define FLM_DEFAULT_PORT 52625
#define LOAD_TIMEOUT_US (30 * 60 * G_USEC_PER_SEC) /* first load may download GBs */

static SoupSession *session;
static SoupSession *probe_session;

static GSubprocess *server;
static gboolean     external;
static guint        generation;
static guint        poll_source;
static gboolean     probing;
static gint64       load_started;
static int          dying;
static gboolean     pending_start;
static char        *pending_model;
static char        *pending_pmode;
static char        *last_log;
static int          pending_io; /* server log reads and waits in flight */
static char        *loaded_model;
static char        *loaded_pmode;
static gboolean     pmode_unsupported;

static FlmState   state = FLM_STOPPED;
static FlmStateCb state_cb;
static gpointer   state_data;

static void spawn_server (const char *model, const char *pmode);

gboolean
flm_available (void)
{
  g_autofree char *path = g_find_program_in_path ("flm");
  return path != NULL;
}

SoupSession *
flm_session (void)
{
  if (!session)
    session = TRACK (soup_session_new_with_options ("timeout", 0, "idle-timeout", 0, NULL));
  return session;
}

/* NPU_CHAT_PORT lets tests run beside a real instance. */
static const char *
port_string (void)
{
  const char *env = g_getenv ("NPU_CHAT_PORT");
  return env && *env ? env : G_STRINGIFY (FLM_DEFAULT_PORT);
}

char *
flm_url (const char *path)
{
  return g_strdup_printf ("http://127.0.0.1:%s%s", port_string (), path);
}

static void
set_state (FlmState s, const char *msg)
{
  state = s;
  if (state_cb)
    state_cb (s, msg, state_data);
}

void
flm_init (FlmStateCb cb, gpointer data)
{
  state_cb = cb;
  state_data = data;
}

gboolean
flm_is_external (void)
{
  return external;
}

FlmState
flm_state (void)
{
  return state;
}

/* Removes ANSI escape sequences and surrounding whitespace in place. */
static char *
clean_line (char *s)
{
  char *w = s;
  for (char *r = s; *r; r++)
    {
      if (*r == '\x1b')
        {
          if (r[1] == '[')
            {
              r += 2;
              while (*r && !g_ascii_isalpha (*r))
                r++;
              if (!*r)
                break;
            }
          continue;
        }
      *w++ = *r;
    }
  *w = '\0';
  return g_strstrip (s);
}

static void
child_setup (gpointer user_data)
{
  (void) user_data;
  /* Do not leave the NPU server running if the app crashes. */
  prctl (PR_SET_PDEATHSIG, SIGTERM);
}

/* ---- probing ---------------------------------------------------------- */

typedef struct {
  FlmBoolCb cb;
  gpointer  data;
} ProbeCtx;

static void
probe_done (GObject *src, GAsyncResult *res, gpointer user_data)
{
  ProbeCtx *ctx = user_data;
  SoupSession *s = SOUP_SESSION (src);
  SoupMessage *msg = soup_session_get_async_result_message (s, res);
  g_autoptr (GBytes) body = soup_session_send_and_read_finish (s, res, NULL);
  gboolean ok = body && msg && soup_message_get_status (msg) == SOUP_STATUS_OK;

  ctx->cb (ok, ctx->data);
  g_free (ctx);
}

static void
probe (FlmBoolCb cb, gpointer data)
{
  if (!probe_session)
    probe_session = TRACK (soup_session_new_with_options ("timeout", 3, NULL));

  g_autofree char *url = flm_url ("/v1/models");
  g_autoptr (SoupMessage) msg = soup_message_new ("GET", url);
  ProbeCtx *ctx = g_new0 (ProbeCtx, 1);
  ctx->cb = cb;
  ctx->data = data;
  soup_session_send_and_read_async (probe_session, msg, G_PRIORITY_DEFAULT, NULL, probe_done, ctx);
}

typedef struct {
  FlmBoolCb cb;
  gpointer  data;
} DetectCtx;

static void
detect_done (gboolean ok, gpointer user_data)
{
  DetectCtx *ctx = user_data;
  external = ok;
  if (ok)
    set_state (FLM_READY, "Usando un servidor FLM que ya estaba abierto");
  ctx->cb (ok, ctx->data);
  g_free (ctx);
}

void
flm_detect (FlmBoolCb cb, gpointer data)
{
  DetectCtx *ctx = g_new0 (DetectCtx, 1);
  ctx->cb = cb;
  ctx->data = data;
  probe (detect_done, ctx);
}

/* ---- server process --------------------------------------------------- */

static void
stop_polling (void)
{
  g_clear_handle_id (&poll_source, g_source_remove);
}

static void
poll_done (gboolean ok, gpointer user_data)
{
  guint gen = GPOINTER_TO_UINT (user_data);
  probing = FALSE;
  if (ok && gen == generation && state == FLM_LOADING)
    {
      stop_polling ();
      set_state (FLM_READY, NULL);
    }
}

static gboolean
poll_tick (gpointer user_data)
{
  guint gen = GPOINTER_TO_UINT (user_data);

  if (gen != generation)
    return G_SOURCE_REMOVE;

  if (g_get_monotonic_time () - load_started > LOAD_TIMEOUT_US)
    {
      poll_source = 0;
      flm_stop ();
      set_state (FLM_ERROR, "El modelo tardó demasiado en cargar");
      return G_SOURCE_REMOVE;
    }

  if (!probing)
    {
      probing = TRUE;
      probe (poll_done, user_data);
    }
  return G_SOURCE_CONTINUE;
}

static void
on_log_line (GObject *src, GAsyncResult *res, gpointer user_data)
{
  GDataInputStream *ds = G_DATA_INPUT_STREAM (src);
  guint gen = GPOINTER_TO_UINT (user_data);
  g_autofree char *raw = g_data_input_stream_read_line_finish (ds, res, NULL, NULL);

  if (!raw)
    {
      pending_io--;
      g_object_unref (ds);
      return;
    }

  g_autofree char *line = g_utf8_make_valid (raw, -1);
  clean_line (line);

  if (*line)
    {
      if (g_getenv ("NPU_CHAT_DEBUG"))
        g_printerr ("[flm] %s\n", line);
      if (gen == generation)
        {
          g_free (last_log);
          last_log = g_strdup (line);
          if (state == FLM_LOADING)
            set_state (FLM_LOADING, line);
        }
    }

  /* Keep draining the pipe, otherwise the server blocks on a full buffer. */
  g_data_input_stream_read_line_async (ds, G_PRIORITY_DEFAULT, NULL, on_log_line, user_data);
}

static void
on_server_exit (GObject *src, GAsyncResult *res, gpointer user_data)
{
  GSubprocess *proc = G_SUBPROCESS (src);
  guint gen = GPOINTER_TO_UINT (user_data);

  g_subprocess_wait_finish (proc, res, NULL);
  pending_io--;

  if (g_object_get_data (G_OBJECT (proc), "dying"))
    {
      dying--;
      if (dying == 0 && pending_start)
        {
          pending_start = FALSE;
          spawn_server (pending_model, pending_pmode);
        }
      return;
    }

  if (gen == generation && server == proc)
    {
      g_clear_object (&server);
      stop_polling ();

      /* Older flm builds may not accept --pmode on "serve": retry once
       * without it instead of failing. */
      if (state == FLM_LOADING && loaded_pmode && !pmode_unsupported && last_log &&
          g_regex_match_simple ("pmode|unknown|unrecogni[sz]ed|invalid|usage", last_log,
                                G_REGEX_CASELESS, 0))
        {
          pmode_unsupported = TRUE;
          g_autofree char *model = g_strdup (loaded_model);
          spawn_server (model, NULL);
          return;
        }

      g_autofree char *msg = g_strdup_printf ("FastFlowLM se cerró: %s",
                                              last_log ? last_log : "sin detalles");
      set_state (FLM_ERROR, msg);
    }
}

static gboolean
force_kill (gpointer user_data)
{
  g_subprocess_force_exit (G_SUBPROCESS (user_data));
  return G_SOURCE_REMOVE;
}

static void
kill_server (void)
{
  if (!server)
    return;

  generation++;
  stop_polling ();
  g_object_set_data (G_OBJECT (server), "dying", GINT_TO_POINTER (1));
  dying++;
  g_subprocess_send_signal (server, SIGTERM);
  g_timeout_add_seconds_full (G_PRIORITY_DEFAULT, 5, force_kill, g_object_ref (server), g_object_unref);
  g_clear_object (&server);
}

static void
spawn_server (const char *model, const char *pmode)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GSubprocessLauncher) launcher = NULL;
  g_autoptr (GPtrArray) argv = g_ptr_array_new ();

  g_ptr_array_add (argv, "flm");
  g_ptr_array_add (argv, "serve");
  g_ptr_array_add (argv, (char *) model);
  g_ptr_array_add (argv, "--port");
  g_ptr_array_add (argv, (char *) port_string ());
  if (pmode_unsupported)
    pmode = NULL;
  if (pmode && *pmode)
    {
      g_ptr_array_add (argv, "--pmode");
      g_ptr_array_add (argv, (char *) pmode);
    }
  g_ptr_array_add (argv, NULL);

  /* stdin stays an open pipe so an interactive server never sees EOF. */
  launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_STDIN_PIPE |
                                        G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                        G_SUBPROCESS_FLAGS_STDERR_MERGE);
  g_subprocess_launcher_set_child_setup (launcher, child_setup, NULL, NULL);

  generation++;
  guint gen = generation;
  g_clear_pointer (&last_log, g_free);
  g_free (loaded_model);
  g_free (loaded_pmode);
  loaded_model = g_strdup (model);
  loaded_pmode = pmode && *pmode ? g_strdup (pmode) : NULL;

  server = g_subprocess_launcher_spawnv (launcher, (const char *const *) argv->pdata, &error);
  if (server)
    (void) TRACK (server);
  if (!server)
    {
      set_state (FLM_ERROR, error->message);
      return;
    }

  GDataInputStream *ds = TRACK (g_data_input_stream_new (g_subprocess_get_stdout_pipe (server)));
  /* Progress bars use bare \r, treat it as a line break too. */
  g_data_input_stream_set_newline_type (ds, G_DATA_STREAM_NEWLINE_TYPE_ANY);
  pending_io += 2;
  g_data_input_stream_read_line_async (ds, G_PRIORITY_DEFAULT, NULL, on_log_line, GUINT_TO_POINTER (gen));
  g_subprocess_wait_async (server, NULL, on_server_exit, GUINT_TO_POINTER (gen));

  load_started = g_get_monotonic_time ();
  set_state (FLM_LOADING, "Iniciando FastFlowLM…");
  poll_source = g_timeout_add (800, poll_tick, GUINT_TO_POINTER (gen));
}

void
flm_load (const char *model, const char *pmode)
{
  if (external)
    {
      set_state (FLM_READY, NULL);
      return;
    }

  g_free (pending_model);
  g_free (pending_pmode);
  pending_model = g_strdup (model);
  pending_pmode = g_strdup (pmode);

  kill_server ();

  if (dying > 0)
    {
      /* The old server still holds the port; start once it has exited. */
      pending_start = TRUE;
      set_state (FLM_LOADING, "Cambiando de modelo…");
      return;
    }

  spawn_server (model, pmode);
}

void
flm_stop (void)
{
  pending_start = FALSE;
  if (external)
    return;
  kill_server ();
  g_clear_pointer (&loaded_model, g_free);
  g_clear_pointer (&loaded_pmode, g_free);
  set_state (FLM_STOPPED, NULL);
}

const char *
flm_loaded_model (void)
{
  return loaded_model;
}

const char *
flm_loaded_pmode (void)
{
  return loaded_pmode;
}

void
flm_shutdown (void)
{
  state_cb = NULL;
  pending_start = FALSE;
  stop_polling ();
  generation++;
  GSubprocess *proc = server ? g_object_ref (server) : NULL;
  if (proc)
    g_subprocess_send_signal (proc, SIGTERM);
  g_clear_object (&server);

  /* Wait for flm to exit so its process and pipes are released cleanly;
   * force it after 3 s. Callbacks see a stale generation and do nothing. */
  gint64 start = g_get_monotonic_time ();
  gboolean forced = FALSE;
  while (pending_io > 0 && g_get_monotonic_time () - start < 6 * G_USEC_PER_SEC)
    {
      if (!g_main_context_iteration (NULL, FALSE))
        g_usleep (10000);
      if (proc && !forced && g_get_monotonic_time () - start > 3 * G_USEC_PER_SEC)
        {
          forced = TRUE;
          g_subprocess_force_exit (proc);
        }
    }
  g_clear_object (&proc);
  g_clear_pointer (&pending_model, g_free);
  g_clear_pointer (&pending_pmode, g_free);
  g_clear_pointer (&last_log, g_free);
  g_clear_pointer (&loaded_model, g_free);
  g_clear_pointer (&loaded_pmode, g_free);
  g_clear_object (&session);
  g_clear_object (&probe_session);
}

/* ---- model list ------------------------------------------------------- */

void
flm_model_free (gpointer p)
{
  FlmModel *m = p;
  g_free (m->name);
  g_strfreev (m->labels);
  g_free (m);
}

static FlmModel *
add_model (GPtrArray *out, const char *name)
{
  if (!name || !*name)
    return NULL;
  for (guint i = 0; i < out->len; i++)
    if (g_str_equal (((FlmModel *) out->pdata[i])->name, name))
      return NULL;
  FlmModel *m = g_new0 (FlmModel, 1);
  m->name = g_strdup (name);
  g_ptr_array_add (out, m);
  return m;
}

/* "9B" -> 9, "300M" -> 0.3 (billions of parameters). */
static double
parse_params (const char *s)
{
  if (!s)
    return 0;
  char *end = NULL;
  double v = g_ascii_strtod (s, &end);
  if (end == s)
    return 0;
  if (*end == 'M' || *end == 'm')
    return v / 1000.0;
  return v;
}

/* Schema of `flm list --json`:
 *   { "models": [ { "name": "qwen3:8b", "footprint": 5.6,
 *                   "details": { "parameter_size": "8B" },
 *                   "label": [ "reasoning", ... ] }, ... ] }
 * "footprint" is the memory the loaded model needs, in GB. */
static void
collect_models (JsonNode *root, GPtrArray *out)
{
  if (!root || !JSON_NODE_HOLDS_OBJECT (root))
    return;
  JsonNode *list = json_object_get_member (json_node_get_object (root), "models");
  if (!list || !JSON_NODE_HOLDS_ARRAY (list))
    return;

  JsonArray *arr = json_node_get_array (list);
  for (guint i = 0; i < json_array_get_length (arr); i++)
    {
      JsonNode *el = json_array_get_element (arr, i);
      if (!JSON_NODE_HOLDS_OBJECT (el))
        continue;
      JsonObject *o = json_node_get_object (el);
      FlmModel *m = add_model (out, json_object_get_string_member_with_default (o, "name", NULL));
      if (!m)
        continue;

      JsonNode *fp = json_object_get_member (o, "footprint");
      if (fp && JSON_NODE_HOLDS_VALUE (fp))
        m->footprint_gb = json_node_get_double (fp);

      JsonNode *details = json_object_get_member (o, "details");
      if (details && JSON_NODE_HOLDS_OBJECT (details))
        m->params_b = parse_params (json_object_get_string_member_with_default (
          json_node_get_object (details), "parameter_size", NULL));

      JsonNode *labels = json_object_get_member (o, "label");
      if (labels && JSON_NODE_HOLDS_ARRAY (labels))
        {
          JsonArray *la = json_node_get_array (labels);
          GStrvBuilder *b = g_strv_builder_new ();
          for (guint j = 0; j < json_array_get_length (la); j++)
            {
              JsonNode *n = json_array_get_element (la, j);
              if (JSON_NODE_HOLDS_VALUE (n) && json_node_get_value_type (n) == G_TYPE_STRING)
                g_strv_builder_add (b, json_node_get_string (n));
            }
          m->labels = g_strv_builder_end (b);
          g_strv_builder_unref (b);
        }
    }
}

static void
collect_from_text (const char *text, GPtrArray *out)
{
  g_autoptr (GRegex) re = g_regex_new ("\\b([a-z][a-z0-9.\\-]*:[a-z0-9][a-z0-9.\\-]*)\\b", 0, 0, NULL);
  g_autoptr (GMatchInfo) mi = NULL;

  g_regex_match (re, text, 0, &mi);
  while (g_match_info_matches (mi))
    {
      g_autofree char *m = g_match_info_fetch (mi, 1);
      add_model (out, m);
      g_match_info_next (mi, NULL);
    }
}

static int
by_name (gconstpointer a, gconstpointer b)
{
  return g_strcmp0 ((*(FlmModel **) a)->name, (*(FlmModel **) b)->name);
}

typedef struct {
  FlmListCb cb;
  gpointer  data;
} ListCtx;

static void
list_done (GObject *src, GAsyncResult *res, gpointer user_data)
{
  ListCtx *ctx = user_data;
  g_autoptr (GError) error = NULL;
  g_autofree char *out = NULL;
  g_autoptr (GPtrArray) names = g_ptr_array_new_with_free_func (flm_model_free);

  if (!g_subprocess_communicate_utf8_finish (G_SUBPROCESS (src), res, &out, NULL, &error))
    {
      ctx->cb (NULL, error->message, ctx->data);
      g_free (ctx);
      return;
    }

  const char *json = out ? strpbrk (out, "[{") : NULL;
  g_autoptr (JsonParser) parser = json_parser_new ();
  if (json && json_parser_load_from_data (parser, json, -1, NULL))
    collect_models (json_parser_get_root (parser), names);
  else if (out)
    collect_from_text (out, names);

  g_ptr_array_sort (names, by_name);
  ctx->cb (names, NULL, ctx->data);
  g_free (ctx);
}

void
flm_list_models (const char *filter, FlmListCb cb, gpointer data)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GSubprocess) proc = g_subprocess_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                                   G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                                                   &error, "flm", "list", "--json",
                                                   "--filter", filter, NULL);
  if (!proc)
    {
      cb (NULL, error->message, data);
      return;
    }

  ListCtx *ctx = g_new0 (ListCtx, 1);
  ctx->cb = cb;
  ctx->data = data;
  g_subprocess_communicate_utf8_async (proc, NULL, NULL, list_done, ctx);
}

/* ---- pull / remove ---------------------------------------------------- */

typedef struct {
  GSubprocess  *proc;
  FlmProgressCb progress;
  FlmDoneCb     done;
  gpointer      data;
  char         *last;
  GRegex       *percent;
} PullCtx;

static void
pull_ctx_free (PullCtx *ctx)
{
  g_object_unref (ctx->proc);
  g_free (ctx->last);
  g_regex_unref (ctx->percent);
  g_free (ctx);
}

static void
pull_exited (GObject *src, GAsyncResult *res, gpointer user_data)
{
  PullCtx *ctx = user_data;
  g_autoptr (GError) error = NULL;
  gboolean ok = g_subprocess_wait_check_finish (G_SUBPROCESS (src), res, &error);

  ctx->done (ok, ok ? ctx->last : (ctx->last ? ctx->last : error->message), ctx->data);
  pull_ctx_free (ctx);
}

static void
pull_line (GObject *src, GAsyncResult *res, gpointer user_data)
{
  PullCtx *ctx = user_data;
  GDataInputStream *ds = G_DATA_INPUT_STREAM (src);
  g_autofree char *raw = g_data_input_stream_read_line_finish (ds, res, NULL, NULL);

  if (!raw)
    {
      g_object_unref (ds);
      g_subprocess_wait_check_async (ctx->proc, NULL, pull_exited, ctx);
      return;
    }

  g_autofree char *line = g_utf8_make_valid (raw, -1);
  clean_line (line);

  if (*line)
    {
      double fraction = -1;
      g_autoptr (GMatchInfo) mi = NULL;
      if (g_regex_match (ctx->percent, line, 0, &mi))
        {
          g_autofree char *num = NULL;
          /* Use the last percentage on the line (overall progress). */
          do
            {
              g_free (num);
              num = g_match_info_fetch (mi, 1);
            }
          while (g_match_info_next (mi, NULL));
          fraction = CLAMP (g_ascii_strtod (num, NULL) / 100.0, 0.0, 1.0);
        }
      g_free (ctx->last);
      ctx->last = g_strdup (line);
      ctx->progress (line, fraction, ctx->data);
    }

  g_data_input_stream_read_line_async (ds, G_PRIORITY_DEFAULT, NULL, pull_line, ctx);
}

void
flm_pull (const char *model, FlmProgressCb progress, FlmDoneCb done, gpointer data)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GSubprocessLauncher) launcher =
    g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_MERGE);
  g_subprocess_launcher_set_child_setup (launcher, child_setup, NULL, NULL);

  GSubprocess *proc = g_subprocess_launcher_spawn (launcher, &error, "flm", "pull", model, NULL);
  if (proc)
    (void) TRACK (proc);
  if (!proc)
    {
      done (FALSE, error->message, data);
      return;
    }

  PullCtx *ctx = g_new0 (PullCtx, 1);
  ctx->proc = proc;
  ctx->progress = progress;
  ctx->done = done;
  ctx->data = data;
  ctx->percent = g_regex_new ("(\\d+(?:\\.\\d+)?)\\s*%", 0, 0, NULL);

  GDataInputStream *ds = g_data_input_stream_new (g_subprocess_get_stdout_pipe (proc));
  g_data_input_stream_set_newline_type (ds, G_DATA_STREAM_NEWLINE_TYPE_ANY);
  g_data_input_stream_read_line_async (ds, G_PRIORITY_DEFAULT, NULL, pull_line, ctx);
}

typedef struct {
  FlmDoneCb done;
  gpointer  data;
} RemoveCtx;

static void
remove_done (GObject *src, GAsyncResult *res, gpointer user_data)
{
  RemoveCtx *ctx = user_data;
  g_autoptr (GError) error = NULL;
  g_autofree char *out = NULL;
  GSubprocess *proc = G_SUBPROCESS (src);

  if (!g_subprocess_communicate_utf8_finish (proc, res, &out, NULL, &error))
    ctx->done (FALSE, error->message, ctx->data);
  else
    ctx->done (g_subprocess_get_successful (proc), out ? g_strstrip (out) : "", ctx->data);
  g_free (ctx);
}

void
flm_remove (const char *model, FlmDoneCb done, gpointer data)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GSubprocess) proc = g_subprocess_new (G_SUBPROCESS_FLAGS_STDIN_PIPE |
                                                   G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                                   G_SUBPROCESS_FLAGS_STDERR_MERGE,
                                                   &error, "flm", "remove", model, NULL);
  if (!proc)
    {
      done (FALSE, error->message, data);
      return;
    }

  RemoveCtx *ctx = g_new0 (RemoveCtx, 1);
  ctx->done = done;
  ctx->data = data;
  /* Answer "yes" in case flm asks for confirmation. */
  g_subprocess_communicate_utf8_async (proc, "y\n", NULL, remove_done, ctx);
}
