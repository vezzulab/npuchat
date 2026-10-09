#include <adwaita.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

#include "assistants.h"
#include "catalog.h"
#include "flm.h"
#include "i18n.h"
#include "markdown.h"
#include "power.h"
#include "rag.h"
#include "selftest.h"
#include "skills.h"
#include "copilotkey.h"
#include "calendar.h"
#include "calendar-alerts.h"
#include "calendar-subscribe.h"
#include "calendar-ui.h"
#include "store.h"
#include "templates.h"
#include "updater.h"
#include "sysinfo.h"

#define APP_ID "io.github.vezzulab.NpuChat"
#define IDLE_UNLOAD_SECONDS (10 * 60)

/* An assistant reply being streamed or shown. root holds a strong ref so the
 * widgets survive if the chat view is cleared while tokens still arrive. */
typedef struct {
  GtkWidget    *root;
  GtkWidget    *think_expander;
  GtkWidget    *think_label;
  GtkWidget    *body;
  GtkWidget    *typing;
  GtkWidget    *footer;
  GtkWidget    *stats;
  GtkWidget    *copy;
  GString      *text;
  GString      *reasoning;
  Conversation *conv;
  gboolean      detached;
  GtkWidget    *avatar;
  GtkWidget    *title;
  GtkWidget    *sources; /* expander with the passages used, NULL until known */
  GtkWidget    *tools_box; /* one line per skill used, NULL until the first one */
  GPtrArray    *steps;   /* ToolStep*: skills used for this reply */
  guint         next_step;
  guint         rounds;  /* how many times the model asked for tools */
  gboolean      no_tools; /* stop offering tools: the model must answer now */
  guint         serial;  /* tells a late skill result which reply it belongs to */
  int           member; /* team chats: index into conv->team, -1 until routed */
} Reply;

/* A skill the model asked for, and what came back. */
typedef struct {
  char      *call_id;
  char      *name;
  char      *args;
  char      *result;
  gboolean   ok;
  gboolean   done;
  GtkWidget *label;
} ToolStep;

/* A tool call being assembled from the streamed reply. */
typedef struct {
  char    *id;
  char    *name;
  GString *args;
} PendingCall;

typedef struct {
  guint serial;
  guint index;
} ToolCtx;

#define MAX_TOOL_ROUNDS 4

typedef struct {
  AdwDialog    *dialog;
  AdwViewStack *stack;
  GtkListBox   *installed;
  GtkListBox   *available;
  GtkStack     *available_stack;
  GtkLabel     *available_msg;
  GtkLabel     *hidden;
  SysInfo      *sys;
  guint         gen;
} ModelsDialog;

static struct {
  GtkWindow              *win;
  AdwToastOverlay        *toasts;
  AdwNavigationSplitView *split;
  GtkListBox             *chat_list;
  GtkStack               *chat_list_stack;
  GtkMenuButton          *model_button;
  GtkLabel               *model_label;
  GtkLabel               *state_label;
  GtkWidget              *state_dot;
  GtkListBox             *picker;
  GtkImage               *power_icon;
  GtkLabel               *power_label;
  GtkStack               *chat_stack;
  AdwStatusPage          *welcome;
  GtkMenuButton          *assistant_button;
  GtkLabel               *assistant_label;
  GtkListBox             *assistant_list;
  GtkSwitch              *team_switch;
  GtkWidget              *clear_assistant;
  GtkWidget              *suggestions;
  GtkWidget              *messages;
  GtkScrolledWindow      *scroller;
  GtkTextView            *input;
  GtkWidget              *placeholder;
  GtkButton              *send;
  AdwBanner              *banner;
  AdwDialog              *prefs;
  ModelsDialog            md;

  /* settings */
  char    *model;
  char    *pmode;
  char    *lang;
  char    *theme;
  gboolean idle_unload;
  gboolean concise;
  gboolean auto_update;
  gint64   last_update_check;
  int      width;
  int      height;

  gboolean      flm_present;
  GPtrArray    *installed; /* FlmModel*, NULL until listed */
  GPtrArray    *convs;     /* Conversation*, newest first */
  GPtrArray    *assistants;
  char         *pick_id;   /* assistant for the next new chat; NULL = general */
  gboolean      team_mode; /* the assistant picker selects several */
  GPtrArray    *pick_team; /* char* assistant ids for the next team chat */
  GArray       *queue;     /* team members (int) still to answer this turn */
  GPtrArray    *calls;     /* PendingCall*: tool calls in the reply being streamed */
  GHashTable   *skills_on; /* ids of the enabled skills */
  guint         reply_serial;
  GPtrArray    *libraries; /* RagLibrary*: every document library */
  RagIndex     *rag_index; /* index of the libraries the open chat uses */
  char         *rag_key;   /* what rag_index was built from */
  GtkMenuButton *docs_button;
  GtkListBox   *docs_list;
  GtkWidget    *import_revealer;
  GtkLabel     *import_label;
  GtkWidget    *import_bar;
  AdwDialog    *libs_dialog;
  AdwDialog    *skills_dialog;
  gboolean      team_answered; /* someone has answered the current user message */
  gint64        search_start;  /* when document search began (µs), 0 if none */
  double        search_secs;   /* how long it took, shown with the reply stats */
  Conversation *conv;      /* current; owned by convs once saved */
  gboolean      conv_saved;
  GHashTable   *pulling;   /* names being downloaded */
  gboolean      lang_changed;
  UpdateInfo   *update;        /* newer release found, if any */
  AdwDialog    *update_dialog;
  GtkWidget    *update_bar;
  gboolean      restart_after_exit;
  gboolean      quitting;

  /* streaming */
  Reply        *reply;
  GCancellable *cancel;
  gboolean      waiting_for_model;
  gboolean      power_reload_pending;
  guint         render_id;
  guint         idle_id;
  gboolean      stick_bottom;
  gint64        t_start;
  gint64        t_first;
  int           n_chunks;
  int           usage_tokens;
  guint         http_status;
  GString      *http_error;
} A;

static void build_ui (void);
static void refresh_models (void);
static void refresh_chat_list (void);
static void render_conversation (void);
static void update_header (const char *message);
static void open_models_dialog (gboolean download);
static void md_refresh (void);
static void update_assistant_ui (void);
static void refresh_docs_ui (void);
static void open_libraries_dialog (void);
static void libs_dialog_refresh (void);
static void begin_retrieval (void);
static void run_tool_round (void);
static void start_request (void);
static void tool_step_free (gpointer p);
static char *library_subtitle (const RagLibrary *lib);
static void answer_next_in_queue (void);
static const char *skip_speaker_tag (const char *text);
static gboolean could_become_pass (const char *text);
static void open_assistant_editor (Assistant *a);
static void open_gallery (void);
static void on_gallery_clicked (GtkButton *button, gpointer user_data);
#ifdef NPU_CHAT_SELFTEST
static gboolean selftest_tick (gpointer user_data);
#endif

/* ---- settings --------------------------------------------------------- */

static char *
settings_path (void)
{
  return g_build_filename (g_get_user_config_dir (), "npu-chat", "settings.ini", NULL);
}

static char *
kf_string (GKeyFile *kf, const char *key, const char *fallback)
{
  char *v = g_key_file_get_string (kf, "chat", key, NULL);
  return v ? v : g_strdup (fallback);
}

static void
settings_load (void)
{
  g_autoptr (GKeyFile) kf = g_key_file_new ();
  g_autofree char *path = settings_path ();
  g_autoptr (GError) error = NULL;
  gboolean ok = g_key_file_load_from_file (kf, path, G_KEY_FILE_NONE, NULL);

  A.model = ok ? kf_string (kf, "model", "") : g_strdup ("");
  A.pmode = ok ? kf_string (kf, "pmode", "auto") : g_strdup ("auto");
  A.lang = ok ? kf_string (kf, "language", "es") : g_strdup ("es");
  A.theme = ok ? kf_string (kf, "theme", "system") : g_strdup ("system");
  A.idle_unload = ok ? g_key_file_get_boolean (kf, "chat", "idle-unload", &error) : TRUE;
  if (error)
    A.idle_unload = TRUE;
  g_clear_error (&error);
  A.concise = ok ? g_key_file_get_boolean (kf, "chat", "concise", &error) : TRUE;
  if (error)
    A.concise = TRUE;
  g_clear_error (&error);
  /* Skills: the saved list wins; a fresh install gets the default set. */
  A.skills_on = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  g_auto (GStrv) saved_skills = ok ? g_key_file_get_string_list (kf, "skills", "enabled", NULL, NULL) : NULL;
  if (saved_skills)
    for (int i = 0; saved_skills[i]; i++)
      {
        if (skill_find (saved_skills[i]))
          g_hash_table_add (A.skills_on, g_strdup (saved_skills[i]));
      }
  else
    {
      guint n;
      const Skill *all = skills_list (&n);
      for (guint i = 0; i < n; i++)
        if (all[i].default_on)
          g_hash_table_add (A.skills_on, g_strdup (all[i].id));
    }
  g_clear_error (&error);
  A.auto_update = ok ? g_key_file_get_boolean (kf, "updates", "automatic", &error) : TRUE;
  if (error)
    A.auto_update = TRUE;
  A.last_update_check = ok ? g_key_file_get_int64 (kf, "updates", "last-check", NULL) : 0;
  A.width = ok ? g_key_file_get_integer (kf, "window", "width", NULL) : 0;
  A.height = ok ? g_key_file_get_integer (kf, "window", "height", NULL) : 0;
  if (A.width < 360 || A.height < 360)
    {
      A.width = 1100;
      A.height = 760;
    }
  if (!*A.model)
    g_clear_pointer (&A.model, g_free);
}

static void
settings_save (void)
{
  g_autoptr (GKeyFile) kf = g_key_file_new ();
  g_autofree char *path = settings_path ();
  g_autofree char *dir = g_path_get_dirname (path);

  g_key_file_set_string (kf, "chat", "model", A.model ? A.model : "");
  g_key_file_set_string (kf, "chat", "pmode", A.pmode);
  g_key_file_set_string (kf, "chat", "language", A.lang);
  g_key_file_set_string (kf, "chat", "theme", A.theme);
  g_key_file_set_boolean (kf, "chat", "idle-unload", A.idle_unload);
  g_key_file_set_boolean (kf, "chat", "concise", A.concise);
  {
    g_autoptr (GPtrArray) on = g_ptr_array_new ();
    guint n;
    const Skill *all = skills_list (&n);
    for (guint i = 0; i < n; i++)
      if (g_hash_table_contains (A.skills_on, all[i].id))
        g_ptr_array_add (on, (gpointer) all[i].id);
    g_key_file_set_string_list (kf, "skills", "enabled", (const char *const *) on->pdata, on->len);
  }
  g_key_file_set_boolean (kf, "updates", "automatic", A.auto_update);
  g_key_file_set_int64 (kf, "updates", "last-check", A.last_update_check);
  g_key_file_set_integer (kf, "window", "width", A.width);
  g_key_file_set_integer (kf, "window", "height", A.height);
  g_mkdir_with_parents (dir, 0700);
  g_key_file_save_to_file (kf, path, NULL);
}

static void
apply_theme (void)
{
  AdwColorScheme scheme = ADW_COLOR_SCHEME_DEFAULT;
  if (g_str_equal (A.theme, "light"))
    scheme = ADW_COLOR_SCHEME_FORCE_LIGHT;
  else if (g_str_equal (A.theme, "dark"))
    scheme = ADW_COLOR_SCHEME_FORCE_DARK;
  adw_style_manager_set_color_scheme (adw_style_manager_get_default (), scheme);
}

/* ---- small helpers ---------------------------------------------------- */

static void toast (const char *fmt, ...) G_GNUC_PRINTF (1, 2);

static void
toast (const char *fmt, ...)
{
  if (!A.toasts)
    return;
  va_list args;
  va_start (args, fmt);
  g_autofree char *msg = g_strdup_vprintf (fmt, args);
  va_end (args);
  AdwToast *t = adw_toast_new (msg);
  adw_toast_set_use_markup (t, FALSE);
  adw_toast_overlay_add_toast (A.toasts, t);
}

static const char *
effective_pmode (void)
{
  if (!A.pmode || !*A.pmode)
    return NULL;
  if (g_str_equal (A.pmode, "auto"))
    return power_on_battery () ? "powersaver" : "performance";
  return A.pmode;
}

/* The NPU pins model weights in RAM. Fedora's systemd user session caps
 * locked memory at 8 MB unless configured, and then every load fails. */
static gboolean
memlock_ok (void)
{
  struct rlimit rl;
  if (getrlimit (RLIMIT_MEMLOCK, &rl) != 0)
    return TRUE;
  return rl.rlim_cur == RLIM_INFINITY || rl.rlim_cur >= (rlim_t) 16 << 30;
}

static const char memlock_commands[] =
  "sudo mkdir -p /etc/systemd/system/user@.service.d /etc/systemd/user.conf.d\n"
  "printf '[Service]\\nLimitMEMLOCK=infinity\\n' | sudo tee /etc/systemd/system/user@.service.d/memlock.conf\n"
  "printf '[Manager]\\nDefaultLimitMEMLOCK=infinity\\n' | sudo tee /etc/systemd/user.conf.d/memlock.conf";

static void
on_memlock_response (AdwAlertDialog *dialog, const char *response, gpointer user_data)
{
  (void) user_data;
  if (g_str_equal (response, "copy"))
    {
      gdk_clipboard_set_text (gtk_widget_get_clipboard (GTK_WIDGET (dialog)), memlock_commands);
      toast ("%s", TR ("Comandos copiados. Pégalos en Konsole y reinicia.",
                       "Commands copied. Paste them into Konsole and restart."));
    }
}

static void
show_memlock_dialog (void)
{
  AdwDialog *d = TRACK (adw_alert_dialog_new (TR ("Falta un paso para usar la NPU", "One step left to use the NPU"), NULL));
  adw_alert_dialog_set_body (ADW_ALERT_DIALOG (d),
                             TR ("La NPU necesita reservar memoria para el modelo, y tu sesión solo le permite 8 MB. "
                                 "Pega estos comandos en Konsole (te pedirá tu contraseña) y luego reinicia el equipo. "
                                 "Solo hay que hacerlo una vez.",
                                 "The NPU needs to reserve memory for the model, but your session only allows 8 MB. "
                                 "Paste these commands into Konsole (it will ask for your password), then restart. "
                                 "You only need to do this once."));
  GtkWidget *cmd = gtk_label_new (memlock_commands);
  gtk_label_set_selectable (GTK_LABEL (cmd), TRUE);
  gtk_label_set_wrap (GTK_LABEL (cmd), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (cmd), PANGO_WRAP_CHAR);
  gtk_label_set_xalign (GTK_LABEL (cmd), 0);
  /* Paths must not get hyphens inserted when wrapped. */
  PangoAttrList *attrs = pango_attr_list_new ();
  pango_attr_list_insert (attrs, pango_attr_insert_hyphens_new (FALSE));
  gtk_label_set_attributes (GTK_LABEL (cmd), attrs);
  pango_attr_list_unref (attrs);
  gtk_widget_add_css_class (cmd, "cmd-card");
  adw_alert_dialog_set_extra_child (ADW_ALERT_DIALOG (d), cmd);
  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (d), "close", TR ("Cerrar", "Close"),
                                  "copy", TR ("Copiar comandos", "Copy commands"), NULL);
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (d), "copy", ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (d), "copy");
  g_signal_connect (d, "response", G_CALLBACK (on_memlock_response), NULL);
  adw_dialog_present (d, GTK_WIDGET (A.win));
}

static const char *
pmode_label (const char *p)
{
  if (!p)
    return TR ("por defecto", "default");
  if (g_str_equal (p, "powersaver"))
    return TR ("ahorro", "power saver");
  if (g_str_equal (p, "balanced"))
    return TR ("equilibrado", "balanced");
  if (g_str_equal (p, "performance"))
    return TR ("rendimiento", "performance");
  if (g_str_equal (p, "turbo"))
    return "turbo";
  return p;
}

static char *
size_text (const ModelInfo *info)
{
  if (info->size_gb <= 0)
    return g_strdup (TR ("tamaño desconocido", "unknown size"));
  double tps = catalog_tokens_per_second (info);
  return g_strdup_printf (TR ("usa %.1f GB · ~%.0f tok/s", "uses %.1f GB · ~%.0f tok/s"), info->size_gb, tps);
}

static FlmModel *
installed_find (const char *name)
{
  for (guint i = 0; A.installed && name && i < A.installed->len; i++)
    {
      FlmModel *m = A.installed->pdata[i];
      if (g_str_equal (m->name, name))
        return m;
    }
  return NULL;
}

static guint
installed_chat_count (void)
{
  guint n = 0;
  for (guint i = 0; A.installed && i < A.installed->len; i++)
    {
      ModelInfo info;
      catalog_describe (A.installed->pdata[i], &info);
      n += info.chat;
    }
  return n;
}

static GtkWidget *
dim_label (const char *text, const char *css)
{
  GtkWidget *l = gtk_label_new (text);
  gtk_label_set_xalign (GTK_LABEL (l), 0);
  gtk_label_set_wrap (GTK_LABEL (l), TRUE);
  gtk_widget_add_css_class (l, "dim-label");
  if (css)
    gtk_widget_add_css_class (l, css);
  return l;
}

/* ---- power & idle ----------------------------------------------------- */

static void
disarm_idle (void)
{
  g_clear_handle_id (&A.idle_id, g_source_remove);
}

static gboolean
idle_unload (gpointer user_data)
{
  (void) user_data;
  A.idle_id = 0;
  if (!A.reply && !flm_is_external () && flm_state () == FLM_READY)
    {
      flm_stop ();
      toast ("%s", TR ("Modelo liberado para ahorrar batería. Se carga solo al escribir.",
                       "Model unloaded to save battery. It reloads when you type."));
    }
  return G_SOURCE_REMOVE;
}

/* On battery, free the NPU and RAM after a while without use. */
static void
arm_idle_timer (void)
{
  disarm_idle ();
  if (A.idle_unload && power_on_battery () && !flm_is_external () &&
      flm_state () == FLM_READY && !A.reply)
    A.idle_id = g_timeout_add_seconds (IDLE_UNLOAD_SECONDS, idle_unload, NULL);
}

static void
reload_for_power (void)
{
  if (!A.model || flm_is_external ())
    return;
  if (flm_state () != FLM_READY && flm_state () != FLM_LOADING)
    return;
  /* NULL means flm rejected --pmode; reloading would change nothing. */
  if (!flm_loaded_pmode () || g_strcmp0 (flm_loaded_pmode (), effective_pmode ()) == 0)
    return;
  flm_load (A.model, effective_pmode ());
}

static void
update_power_bar (void)
{
  if (!A.power_label)
    return;
  gboolean bat = power_on_battery ();
  gtk_image_set_from_icon_name (A.power_icon, bat ? "npu-battery-symbolic" : "npu-plugged-symbolic");
  g_autofree char *text = g_strdup_printf ("%s · NPU %s",
                                           bat ? TR ("Batería", "Battery") : TR ("Conectado", "Plugged in"),
                                           pmode_label (effective_pmode ()));
  gtk_label_set_text (A.power_label, text);
}

static void
on_power_changed (gboolean on_battery, gpointer user_data)
{
  (void) user_data;
  if (A.quitting)
    return;

  arm_idle_timer ();
  update_power_bar ();

  if (g_strcmp0 (A.pmode, "auto") != 0 || flm_is_external ())
    return;

  if (A.reply)
    A.power_reload_pending = TRUE;
  else
    reload_for_power ();

  if (flm_state () == FLM_READY || flm_state () == FLM_LOADING)
    toast ("%s", on_battery ? TR ("En batería: NPU en modo ahorro", "On battery: NPU power saver")
                            : TR ("Conectado: NPU en modo rendimiento", "Plugged in: NPU performance mode"));
}

/* ---- header & state --------------------------------------------------- */

static gboolean
is_streaming (void)
{
  return A.reply != NULL;
}

static void
update_send_button (void)
{
  if (!A.send)
    return;
  gboolean streaming = is_streaming ();
  gtk_button_set_icon_name (A.send, streaming ? "media-playback-stop-symbolic" : "go-up-symbolic");
  gtk_widget_set_tooltip_text (GTK_WIDGET (A.send), streaming ? TR ("Detener", "Stop")
                                                              : TR ("Enviar (Enter)", "Send (Enter)"));
  gtk_widget_set_sensitive (GTK_WIDGET (A.send), streaming || (A.model && A.flm_present));
}

static void
set_state_dot (const char *css)
{
  static const char *all[] = { "ready", "loading", "error", NULL };
  for (int i = 0; all[i]; i++)
    gtk_widget_remove_css_class (A.state_dot, all[i]);
  if (css)
    gtk_widget_add_css_class (A.state_dot, css);
}

static void
update_header (const char *message)
{
  if (!A.model_label)
    return;

  g_autofree char *state = NULL;
  const char *dot = NULL;

  gtk_label_set_text (A.model_label, A.model ? A.model : TR ("Elige un modelo", "Choose a model"));

  if (!A.flm_present)
    state = g_strdup (TR ("FastFlowLM no instalado", "FastFlowLM not installed"));
  else
    switch (flm_state ())
      {
      case FLM_STOPPED:
        state = g_strdup (A.model ? TR ("En reposo · se carga al escribir", "Idle · loads when you type")
                                  : TR ("Sin modelo", "No model"));
        break;
      case FLM_LOADING:
        state = g_strdup (TR ("Cargando en la NPU…", "Loading on the NPU…"));
        dot = "loading";
        break;
      case FLM_READY:
        state = flm_is_external ()
                  ? g_strdup (TR ("Listo · servidor externo", "Ready · external server"))
                  : g_strdup_printf ("%s · NPU %s", TR ("Listo", "Ready"), pmode_label (flm_loaded_pmode ()));
        dot = "ready";
        break;
      case FLM_ERROR:
        state = g_strdup (TR ("Error · toca para reintentar", "Error · tap to retry"));
        dot = "error";
        break;
      }

  if (A.flm_present && !memlock_ok ())
    {
      g_free (state);
      state = g_strdup (TR ("Falta un ajuste del sistema", "System setting needed"));
      dot = "error";
    }
  adw_banner_set_revealed (A.banner, A.flm_present && !memlock_ok ());

  gtk_label_set_text (A.state_label, state);
  gtk_widget_set_tooltip_text (GTK_WIDGET (A.model_button), message);
  set_state_dot (dot);
  update_power_bar ();
  update_send_button ();
}

static void
ensure_loaded (void)
{
  if (!A.model || !A.flm_present || !memlock_ok ())
    return;
  if (flm_state () == FLM_STOPPED || flm_state () == FLM_ERROR)
    flm_load (A.model, effective_pmode ());
}

/* ---- scrolling -------------------------------------------------------- */

static void
on_adj_value (GtkAdjustment *adj, gpointer user_data)
{
  (void) user_data;
  double bottom = gtk_adjustment_get_upper (adj) - gtk_adjustment_get_page_size (adj);
  A.stick_bottom = gtk_adjustment_get_value (adj) >= bottom - 48;
}

static void
on_adj_changed (GtkAdjustment *adj, gpointer user_data)
{
  (void) user_data;
  if (A.stick_bottom)
    gtk_adjustment_set_value (adj, gtk_adjustment_get_upper (adj) - gtk_adjustment_get_page_size (adj));
}

static void
scroll_to_bottom (void)
{
  A.stick_bottom = TRUE;
  on_adj_changed (gtk_scrolled_window_get_vadjustment (A.scroller), NULL);
}

/* ---- message widgets -------------------------------------------------- */

static void
copy_text (GtkWidget *w, const char *text)
{
  gdk_clipboard_set_text (gtk_widget_get_clipboard (w), text ? text : "");
  toast ("%s", TR ("Copiado", "Copied"));
}

static void
on_copy_clicked (GtkButton *button, gpointer user_data)
{
  (void) user_data;
  copy_text (GTK_WIDGET (button), g_object_get_data (G_OBJECT (button), "text"));
}

static GtkWidget *
icon_button (const char *icon, const char *tooltip)
{
  GtkWidget *b = gtk_button_new_from_icon_name (icon);
  gtk_widget_add_css_class (b, "flat");
  gtk_widget_add_css_class (b, "circular");
  gtk_widget_set_valign (b, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text (b, tooltip);
  return b;
}

static GtkWidget *
new_text_block (void)
{
  GtkWidget *label = gtk_label_new (NULL);
  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (label), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_label_set_selectable (GTK_LABEL (label), TRUE);
  gtk_widget_set_focusable (label, FALSE);
  gtk_widget_add_css_class (label, "md");
  g_object_set_data (G_OBJECT (label), "kind", GINT_TO_POINTER (MD_TEXT + 1));
  return label;
}

static GtkWidget *
new_code_block (void)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (box, "codeblock");
  g_object_set_data (G_OBJECT (box), "kind", GINT_TO_POINTER (MD_CODE + 1));

  GtkWidget *header = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class (header, "code-header");
  GtkWidget *lang = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (lang), 0);
  gtk_widget_set_hexpand (lang, TRUE);
  gtk_widget_add_css_class (lang, "code-lang");
  GtkWidget *copy = icon_button ("edit-copy-symbolic", TR ("Copiar", "Copy"));
  g_signal_connect (copy, "clicked", G_CALLBACK (on_copy_clicked), NULL);
  gtk_box_append (GTK_BOX (header), lang);
  gtk_box_append (GTK_BOX (header), copy);

  GtkWidget *code = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (code), 0);
  gtk_label_set_selectable (GTK_LABEL (code), TRUE);
  gtk_widget_set_focusable (code, FALSE);
  gtk_widget_add_css_class (code, "code");

  GtkWidget *sw = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
  gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (sw), TRUE);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (sw), code);

  gtk_box_append (GTK_BOX (box), header);
  gtk_box_append (GTK_BOX (box), sw);
  g_object_set_data (G_OBJECT (box), "lang-label", lang);
  g_object_set_data (G_OBJECT (box), "code-label", code);
  g_object_set_data (G_OBJECT (box), "copy", copy);
  return box;
}

static void
update_block (GtkWidget *w, MdBlock *b)
{
  const char *old = g_object_get_data (G_OBJECT (w), "src");
  if (g_strcmp0 (old, b->text) == 0)
    return;

  if (b->kind == MD_TEXT)
    {
      gtk_label_set_markup (GTK_LABEL (w), b->text);
    }
  else
    {
      GtkLabel *lang = g_object_get_data (G_OBJECT (w), "lang-label");
      GtkLabel *code = g_object_get_data (G_OBJECT (w), "code-label");
      GObject *copy = g_object_get_data (G_OBJECT (w), "copy");
      gtk_label_set_text (lang, b->lang && *b->lang ? b->lang : TR ("código", "code"));
      gtk_label_set_text (code, b->text);
      g_object_set_data_full (copy, "text", g_strdup (b->text), g_free);
    }
  g_object_set_data_full (G_OBJECT (w), "src", g_strdup (b->text), g_free);
}

static void
remove_from (GtkWidget *box, GtkWidget *child)
{
  while (child)
    {
      GtkWidget *next = gtk_widget_get_next_sibling (child);
      gtk_box_remove (GTK_BOX (box), child);
      child = next;
    }
}

/* Updates the reply body in place, reusing widgets so selection and
 * scrolling stay stable while tokens stream in. */
static void
render_markdown_into (GtkWidget *body, const char *markdown)
{
  g_autoptr (GPtrArray) blocks = md_parse (markdown);
  GtkWidget *child = gtk_widget_get_first_child (body);

  for (guint i = 0; i < blocks->len; i++)
    {
      MdBlock *b = blocks->pdata[i];
      int kind = child ? GPOINTER_TO_INT (g_object_get_data (G_OBJECT (child), "kind")) - 1 : -1;

      if (child && kind == (int) b->kind)
        {
          update_block (child, b);
          child = gtk_widget_get_next_sibling (child);
          continue;
        }

      remove_from (body, child);
      child = NULL;
      GtkWidget *w = b->kind == MD_TEXT ? new_text_block () : new_code_block ();
      update_block (w, b);
      gtk_box_append (GTK_BOX (body), w);
    }

  remove_from (body, child);
}

static void
add_user_bubble (const char *text)
{
  GtkWidget *label = gtk_label_new (text);
  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (label), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_label_set_selectable (GTK_LABEL (label), TRUE);
  gtk_label_set_max_width_chars (GTK_LABEL (label), 52);
  gtk_widget_set_focusable (label, FALSE);
  gtk_widget_set_halign (label, GTK_ALIGN_END);
  gtk_widget_add_css_class (label, "bubble");
  gtk_widget_add_css_class (label, "user");
  gtk_box_append (GTK_BOX (A.messages), label);
}

/* emoji/name describe the assistant; NULL means the general one. */
static Reply *
reply_new (const char *model, const char *emoji, const char *name)
{
  Reply *r = g_new0 (Reply, 1);
  r->text = g_string_new (NULL);
  r->reasoning = g_string_new (NULL);
  r->steps = g_ptr_array_new_with_free_func (tool_step_free);
  r->serial = ++A.reply_serial;

  r->root = TRACK (g_object_ref_sink (gtk_box_new (GTK_ORIENTATION_VERTICAL, 8)));
  gtk_widget_add_css_class (r->root, "reply");

  GtkWidget *header = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  r->member = -1;
  GtkWidget *avatar = r->avatar = gtk_label_new (emoji && *emoji ? emoji : "✦");
  gtk_widget_add_css_class (avatar, "avatar");
  gtk_widget_set_valign (avatar, GTK_ALIGN_CENTER);
  GtkWidget *title = r->title = gtk_label_new (name ? name : (model ? model : ""));
  gtk_widget_add_css_class (title, "model-name");
  r->typing = gtk_spinner_new ();
  gtk_spinner_set_spinning (GTK_SPINNER (r->typing), TRUE);
  gtk_box_append (GTK_BOX (header), avatar);
  gtk_box_append (GTK_BOX (header), title);
  if (name && model)
    {
      GtkWidget *sub = gtk_label_new (model);
      gtk_widget_add_css_class (sub, "reply-model");
      gtk_box_append (GTK_BOX (header), sub);
    }
  gtk_box_append (GTK_BOX (header), r->typing);

  r->think_label = gtk_label_new (NULL);
  gtk_label_set_wrap (GTK_LABEL (r->think_label), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (r->think_label), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_xalign (GTK_LABEL (r->think_label), 0);
  gtk_label_set_selectable (GTK_LABEL (r->think_label), TRUE);
  gtk_widget_set_focusable (r->think_label, FALSE);
  gtk_widget_add_css_class (r->think_label, "think-text");
  r->think_expander = gtk_expander_new (NULL);
  gtk_expander_set_child (GTK_EXPANDER (r->think_expander), r->think_label);
  gtk_widget_add_css_class (r->think_expander, "think");
  gtk_widget_set_visible (r->think_expander, FALSE);

  r->body = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_add_css_class (r->body, "reply-body");

  r->footer = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_add_css_class (r->footer, "reply-footer");
  r->stats = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (r->stats), 0);
  gtk_label_set_wrap (GTK_LABEL (r->stats), TRUE);
  gtk_widget_set_hexpand (r->stats, TRUE);
  gtk_widget_add_css_class (r->stats, "stats");
  r->copy = icon_button ("edit-copy-symbolic", TR ("Copiar respuesta", "Copy reply"));
  g_signal_connect (r->copy, "clicked", G_CALLBACK (on_copy_clicked), NULL);
  gtk_box_append (GTK_BOX (r->footer), r->stats);
  gtk_box_append (GTK_BOX (r->footer), r->copy);
  gtk_widget_set_visible (r->footer, FALSE);

  gtk_box_append (GTK_BOX (r->root), header);
  gtk_box_append (GTK_BOX (r->root), r->think_expander);
  gtk_box_append (GTK_BOX (r->root), r->body);
  gtk_box_append (GTK_BOX (r->root), r->footer);
  return r;
}

static void
tool_step_free (gpointer p)
{
  ToolStep *t = p;
  g_free (t->call_id);
  g_free (t->name);
  g_free (t->args);
  g_free (t->result);
  g_free (t);
}

static const char *
skill_title (const char *id)
{
  const Skill *sk = skill_find (id);
  return sk ? TR (sk->name_es, sk->name_en) : id;
}

/* "1847*392+15" for the calculator, "Ada Lovelace" for Wikipedia. */
static char *
tool_subject (const char *args)
{
  g_autoptr (JsonParser) parser = json_parser_new ();
  if (!args || !json_parser_load_from_data (parser, args, -1, NULL) || !JSON_NODE_HOLDS_OBJECT (json_parser_get_root (parser)))
    return g_strdup ("");
  JsonObject *o = json_node_get_object (json_parser_get_root (parser));
  const char *v = json_object_get_string_member_with_default (o, "expression", NULL);
  if (!v)
    v = json_object_get_string_member_with_default (o, "topic", NULL);
  return g_strdup (v ? v : "");
}

static char *
one_line (const char *text, int max_chars)
{
  g_autofree char *flat = g_strdup (text ? text : "");
  for (char *c = flat; *c; c++)
    if (*c == '\n')
      {
        *c = '\0';
        break;
      }
  if (g_utf8_strlen (flat, -1) <= max_chars)
    return g_steal_pointer (&flat);
  g_autofree char *cut = g_utf8_substring (flat, 0, max_chars - 1);
  return g_strconcat (cut, "…", NULL);
}

/* "Calculator   1847*392+15  →  724039" */
static void
tool_label_update (GtkWidget *label, const char *name, const char *args, const char *result, gboolean ok, gboolean running)
{
  g_autofree char *subject = tool_subject (args);
  g_autofree char *subject_line = one_line (subject, 60);
  g_autofree char *res = running ? g_strdup (TR ("consultando…", "working…")) : one_line (result, 90);
  g_autofree char *e_title = g_markup_escape_text (skill_title (name), -1);
  g_autofree char *e_subject = g_markup_escape_text (subject_line, -1);
  g_autofree char *e_res = g_markup_escape_text (res, -1);
  g_autofree char *markup = g_strdup_printf ("<b>%s</b>%s%s  →  %s%s%s", e_title, *e_subject ? "  " : "", e_subject,
                                             ok ? "" : "<span foreground=\"#e5484d\">", e_res, ok ? "" : "</span>");
  gtk_label_set_markup (GTK_LABEL (label), markup);
}

static GtkWidget *
reply_tool_row (Reply *r, const char *name, const char *args, const char *result, gboolean ok, gboolean running)
{
  if (!r->tools_box)
    {
      r->tools_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
      gtk_widget_add_css_class (r->tools_box, "tools");
      gtk_box_insert_child_after (GTK_BOX (r->root), r->tools_box, r->think_expander);
    }
  GtkWidget *label = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_label_set_selectable (GTK_LABEL (label), TRUE);
  gtk_widget_add_css_class (label, "tool-step");
  tool_label_update (label, name, args, result, ok, running);
  gtk_box_append (GTK_BOX (r->tools_box), label);
  return label;
}

static void
reply_set_tools (Reply *r, GPtrArray *tools)
{
  for (guint i = 0; tools && i < tools->len; i++)
    {
      StoreTool *t = tools->pdata[i];
      reply_tool_row (r, t->name, t->args, t->result, t->ok, FALSE);
    }
}

/* Lists the passages the answer was based on, below the reply. */
static void
reply_set_sources (Reply *r, GPtrArray *sources)
{
  if (r->sources)
    {
      gtk_box_remove (GTK_BOX (r->root), r->sources);
      r->sources = NULL;
    }
  if (!sources)
    return;

  if (sources->len == 0)
    {
      r->sources = dim_label (TR ("Sin coincidencias en tus documentos.", "No matches in your documents."), "caption");
      gtk_widget_add_css_class (r->sources, "sources");
      gtk_box_insert_child_after (GTK_BOX (r->root), r->sources, r->body);
      return;
    }

  g_autofree char *title = g_strdup_printf (TR ("Fuentes (%u)", "Sources (%u)"), sources->len);
  GtkWidget *expander = gtk_expander_new (title);
  gtk_widget_add_css_class (expander, "sources");
  GtkWidget *list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_set_margin_top (list, 6);

  for (guint i = 0; i < sources->len; i++)
    {
      StoreSource *src = sources->pdata[i];
      g_autofree char *ref = src->page > 0
        ? g_strdup_printf ("[%u]  %s · %s %d", i + 1, src->file, TR ("pág.", "p."), src->page)
        : g_strdup_printf ("[%u]  %s", i + 1, src->file);
      GtkWidget *name = gtk_label_new (ref);
      gtk_label_set_xalign (GTK_LABEL (name), 0);
      gtk_label_set_ellipsize (GTK_LABEL (name), PANGO_ELLIPSIZE_MIDDLE);
      gtk_widget_add_css_class (name, "source-ref");

      g_autofree char *flat = g_strdup (src->text);
      for (char *c = flat; *c; c++)
        if (*c == '\n')
          *c = ' ';
      GtkWidget *excerpt = gtk_label_new (flat);
      gtk_label_set_xalign (GTK_LABEL (excerpt), 0);
      gtk_label_set_wrap (GTK_LABEL (excerpt), TRUE);
      gtk_label_set_wrap_mode (GTK_LABEL (excerpt), PANGO_WRAP_WORD_CHAR);
      gtk_label_set_lines (GTK_LABEL (excerpt), 3);
      gtk_label_set_ellipsize (GTK_LABEL (excerpt), PANGO_ELLIPSIZE_END);
      gtk_label_set_selectable (GTK_LABEL (excerpt), TRUE);
      gtk_widget_add_css_class (excerpt, "source-text");

      GtkWidget *item = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
      gtk_box_append (GTK_BOX (item), name);
      gtk_box_append (GTK_BOX (item), excerpt);
      gtk_box_append (GTK_BOX (list), item);
    }
  gtk_expander_set_child (GTK_EXPANDER (expander), list);
  r->sources = expander;
  gtk_box_insert_child_after (GTK_BOX (r->root), expander, r->body);
}

static void
reply_free (Reply *r)
{
  g_ptr_array_unref (r->steps);
  g_object_unref (r->root);
  g_string_free (r->text, TRUE);
  g_string_free (r->reasoning, TRUE);
  g_free (r);
}

static void
reply_show (Reply *r, const char *answer, const char *think, gboolean thinking)
{
  if (think && *think)
    {
      gtk_widget_set_visible (r->think_expander, TRUE);
      gtk_expander_set_label (GTK_EXPANDER (r->think_expander),
                              thinking ? TR ("Pensando…", "Thinking…") : TR ("Razonamiento", "Reasoning"));
      gtk_label_set_text (GTK_LABEL (r->think_label), think);
    }
  render_markdown_into (r->body, answer);
}

static void
reply_finish_footer (Reply *r, const char *answer, const char *stats, const char *error)
{
  g_autoptr (GString) line = g_string_new (stats);

  gtk_spinner_set_spinning (GTK_SPINNER (r->typing), FALSE);
  gtk_widget_set_visible (r->typing, FALSE);
  if (error)
    {
      if (line->len)
        g_string_append (line, "   ·   ");
      g_string_append (line, error);
      gtk_widget_add_css_class (r->stats, "error");
    }
  gtk_label_set_text (GTK_LABEL (r->stats), line->str);
  g_object_set_data_full (G_OBJECT (r->copy), "text", g_strdup (answer), g_free);
  gtk_widget_set_visible (r->copy, answer && *answer);
  gtk_widget_set_visible (r->footer, line->len > 0 || (answer && *answer));
}

/* Splits "<think>…</think>answer" from the raw stream. */
static char *
split_thinking (Reply *r, GString *think)
{
  const char *s = r->text->str;
  while (g_ascii_isspace (*s))
    s++;

  g_string_assign (think, r->reasoning->str);

  if (!g_str_has_prefix (s, "<think>"))
    return g_strstrip (g_strdup (s));

  const char *start = s + strlen ("<think>");
  const char *end = strstr (start, "</think>");
  if (!end)
    {
      g_string_append (think, start);
      return g_strdup ("");
    }
  g_string_append_len (think, start, end - start);
  return g_strstrip (g_strdup (end + strlen ("</think>")));
}

static void
render_reply (gboolean final)
{
  Reply *r = A.reply;
  g_autoptr (GString) think = g_string_new (NULL);
  g_autofree char *answer = split_thinking (r, think);
  g_autofree char *think_text = g_strstrip (g_strdup (think->str));
  const char *shown = answer;
  if (r->member >= 0)
    {
      shown = skip_speaker_tag (answer);
      if (could_become_pass (shown))
        shown = "";
    }
  reply_show (r, shown, think_text, !final && *shown == '\0');
}

static gboolean
render_tick (gpointer user_data)
{
  (void) user_data;
  A.render_id = 0;
  if (A.reply && !A.reply->detached)
    render_reply (FALSE);
  return G_SOURCE_REMOVE;
}

static void
queue_render (void)
{
  /* Fewer redraws on battery; the text still feels live. */
  if (!A.render_id)
    A.render_id = g_timeout_add (power_on_battery () ? 120 : 50, render_tick, NULL);
}

/* ---- conversation view ------------------------------------------------ */

static void
clear_messages (void)
{
  if (A.reply)
    A.reply->detached = TRUE;
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (A.messages)))
    gtk_box_remove (GTK_BOX (A.messages), child);
}

static void
show_empty_state (void)
{
  if (!A.flm_present)
    gtk_stack_set_visible_child_name (A.chat_stack, "setup");
  else if (A.installed && installed_chat_count () == 0)
    gtk_stack_set_visible_child_name (A.chat_stack, "nomodels");
  else
    {
      update_assistant_ui ();
      gtk_stack_set_visible_child_name (A.chat_stack, "welcome");
    }
}

static void
render_conversation (void)
{
  clear_messages ();

  if (!A.conv || A.conv->msgs->len == 0)
    {
      show_empty_state ();
      return;
    }

  GPtrArray *turn_sources = NULL; /* of the user message the next replies answer */
  for (guint i = 0; i < A.conv->msgs->len; i++)
    {
      StoreMsg *m = A.conv->msgs->pdata[i];
      if (g_str_equal (m->role, "user"))
        {
          add_user_bubble (m->content);
          turn_sources = m->sources;
          continue;
        }
      Reply *r = m->author ? reply_new (A.conv->model, m->author_emoji, m->author)
                           : reply_new (A.conv->model, A.conv->assistant_emoji, A.conv->assistant_name);
      reply_show (r, m->content, m->think, FALSE);
      reply_finish_footer (r, m->content, m->stats, NULL);
      reply_set_tools (r, m->tools);
      reply_set_sources (r, turn_sources);
      gtk_box_append (GTK_BOX (A.messages), r->root);
      reply_free (r);
    }

  /* A reply still streaming for this chat goes back where it was. */
  if (A.reply && A.reply->conv == A.conv)
    {
      if (!gtk_widget_get_parent (A.reply->root))
        gtk_box_append (GTK_BOX (A.messages), A.reply->root);
      A.reply->detached = FALSE;
      if (turn_sources)
        reply_set_sources (A.reply, turn_sources);
      render_reply (FALSE);
    }

  gtk_stack_set_visible_child_name (A.chat_stack, "chat");
  update_assistant_ui ();
  scroll_to_bottom ();
}

static void
conv_to_front (Conversation *c)
{
  guint idx;
  if (g_ptr_array_find (A.convs, c, &idx) && idx > 0)
    {
      g_ptr_array_steal_index (A.convs, idx);
      g_ptr_array_insert (A.convs, 0, c);
    }
}

static void
drop_rag_index (void)
{
  g_clear_pointer (&A.rag_index, rag_index_free);
  g_clear_pointer (&A.rag_key, g_free);
}

static void
leave_conversation (void)
{
  /* The index is only worth its memory while its chat is open. */
  drop_rag_index ();
  if (!A.conv)
    return;
  if (!A.conv_saved)
    conversation_free (A.conv);
  else if (!(A.reply && A.reply->conv == A.conv))
    store_unload_messages (A.conv);
  A.conv = NULL;
}

static void
new_chat (void)
{
  if (A.conv && !A.conv_saved)
    {
      render_conversation ();
      gtk_list_box_unselect_all (A.chat_list);
      gtk_widget_grab_focus (GTK_WIDGET (A.input));
      return;
    }
  leave_conversation ();
  A.conv = conversation_new ();
  A.conv_saved = FALSE;
  render_conversation ();
  gtk_list_box_unselect_all (A.chat_list);
  adw_navigation_split_view_set_show_content (A.split, TRUE);
  gtk_widget_grab_focus (GTK_WIDGET (A.input));
}

static void
open_conversation (Conversation *c)
{
  adw_navigation_split_view_set_show_content (A.split, TRUE);
  if (c == A.conv)
    return;
  leave_conversation ();
  A.conv = c;
  A.conv_saved = TRUE;
  store_load_messages (c);
  render_conversation ();
}

/* ---- chat list -------------------------------------------------------- */

static int
date_group (gint64 t)
{
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  g_autoptr (GDateTime) midnight = g_date_time_new_local (g_date_time_get_year (now),
                                                           g_date_time_get_month (now),
                                                           g_date_time_get_day_of_month (now), 0, 0, 0);
  gint64 start = g_date_time_to_unix (midnight);
  if (t >= start)
    return 0;
  if (t >= start - 86400)
    return 1;
  if (t >= start - 6 * 86400)
    return 2;
  return 3;
}

static const char *
group_name (int g)
{
  switch (g)
    {
    case 0: return TR ("Hoy", "Today");
    case 1: return TR ("Ayer", "Yesterday");
    case 2: return TR ("Esta semana", "This week");
    default: return TR ("Anteriores", "Older");
    }
}

static void
chat_header_func (GtkListBoxRow *row, GtkListBoxRow *before, gpointer user_data)
{
  (void) user_data;
  Conversation *c = g_object_get_data (G_OBJECT (row), "conv");
  int g = date_group (c->updated);
  int prev = before ? date_group (((Conversation *) g_object_get_data (G_OBJECT (before), "conv"))->updated) : -1;

  if (g == prev)
    {
      gtk_list_box_row_set_header (row, NULL);
      return;
    }
  GtkWidget *l = gtk_label_new (group_name (g));
  gtk_label_set_xalign (GTK_LABEL (l), 0);
  gtk_widget_add_css_class (l, "chat-group");
  gtk_list_box_row_set_header (row, l);
}

static void
on_delete_chat_response (AdwAlertDialog *dialog, const char *response, gpointer user_data)
{
  (void) dialog;
  Conversation *c = user_data;
  guint idx;

  if (!g_str_equal (response, "delete") || !g_ptr_array_find (A.convs, c, &idx))
    return;

  if (A.reply && A.reply->conv == c)
    {
      g_cancellable_cancel (A.cancel);
      A.reply->conv = NULL;
    }
  store_delete (c);
  /* Files that were dropped into this chat go with it. */
  for (guint i = 0; i < c->libs->len; i++)
    {
      RagLibrary *lib = rag_library_find (A.libraries, c->libs->pdata[i]);
      if (lib && lib->chat_scoped && !lib->busy)
        {
          rag_library_delete (lib);
          g_ptr_array_remove (A.libraries, lib);
        }
    }
  drop_rag_index ();
  gboolean current = c == A.conv;
  if (current)
    A.conv = NULL;
  /* Drop the rows first: their header callback still reads the conversation. */
  gtk_list_box_remove_all (A.chat_list);
  g_ptr_array_remove_index (A.convs, idx);
  refresh_chat_list ();
  if (current)
    new_chat ();
}

static void
on_delete_chat_clicked (GtkButton *button, gpointer user_data)
{
  (void) user_data;
  Conversation *c = g_object_get_data (G_OBJECT (button), "conv");
  AdwDialog *d = TRACK (adw_alert_dialog_new (TR ("¿Borrar conversación?", "Delete conversation?"), NULL));
  adw_alert_dialog_set_body (ADW_ALERT_DIALOG (d), c->title ? c->title : "");
  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (d), "cancel", TR ("Cancelar", "Cancel"),
                                  "delete", TR ("Borrar", "Delete"), NULL);
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (d), "delete", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (d), "cancel");
  g_signal_connect (d, "response", G_CALLBACK (on_delete_chat_response), c);
  adw_dialog_present (d, GTK_WIDGET (A.win));
}

static char *
relative_time (gint64 t)
{
  g_autoptr (GDateTime) dt = g_date_time_new_from_unix_local (t);
  switch (date_group (t))
    {
    case 0: return g_date_time_format (dt, "%H:%M");
    case 1: return g_strdup (TR ("ayer", "yesterday"));
    default: return g_date_time_format (dt, "%d/%m");
    }
}

static GtkWidget *
chat_row (Conversation *c)
{
  GtkWidget *row = gtk_list_box_row_new ();
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *text = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_hexpand (text, TRUE);

  GtkWidget *title = gtk_label_new (c->title && *c->title ? c->title : TR ("Sin título", "Untitled"));
  gtk_label_set_xalign (GTK_LABEL (title), 0);
  gtk_label_set_ellipsize (GTK_LABEL (title), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class (title, "chat-title");

  g_autofree char *when = relative_time (c->updated);
  g_autoptr (GString) team_emojis = g_string_new (NULL);
  for (guint i = 0; c->team && i < c->team->len; i++)
    g_string_append_printf (team_emojis, "%s", ((TeamMember *) c->team->pdata[i])->emoji);
  g_autofree char *sub_text = c->team && c->team->len >= 2
    ? g_strdup_printf ("%s %s · %s", team_emojis->str, TR ("Equipo", "Team"), when)
    : c->assistant_name
    ? g_strdup_printf ("%s %s · %s", c->assistant_emoji ? c->assistant_emoji : "", c->assistant_name, when)
    : g_strdup_printf ("%s · %s", c->model && *c->model ? c->model : "—", when);
  GtkWidget *sub = gtk_label_new (sub_text);
  gtk_label_set_xalign (GTK_LABEL (sub), 0);
  gtk_label_set_ellipsize (GTK_LABEL (sub), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class (sub, "chat-sub");

  gtk_box_append (GTK_BOX (text), title);
  gtk_box_append (GTK_BOX (text), sub);

  GtkWidget *del = icon_button ("user-trash-symbolic", TR ("Borrar", "Delete"));
  gtk_widget_add_css_class (del, "row-delete");
  g_object_set_data (G_OBJECT (del), "conv", c);
  g_signal_connect (del, "clicked", G_CALLBACK (on_delete_chat_clicked), NULL);

  gtk_box_append (GTK_BOX (box), text);
  gtk_box_append (GTK_BOX (box), del);
  gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), box);
  g_object_set_data (G_OBJECT (row), "conv", c);
  return row;
}

static void
refresh_chat_list (void)
{
  gtk_list_box_remove_all (A.chat_list);
  for (guint i = 0; i < A.convs->len; i++)
    {
      GtkWidget *row = chat_row (A.convs->pdata[i]);
      gtk_list_box_append (A.chat_list, row);
      if (A.convs->pdata[i] == A.conv)
        gtk_list_box_select_row (A.chat_list, GTK_LIST_BOX_ROW (row));
    }
  gtk_stack_set_visible_child_name (A.chat_list_stack, A.convs->len ? "list" : "empty");
}

static void
on_chat_activated (GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
  (void) box;
  (void) user_data;
  open_conversation (g_object_get_data (G_OBJECT (row), "conv"));
}

/* ---- teams ------------------------------------------------------------ */

static gboolean
conv_is_team (Conversation *c)
{
  return c && c->team && c->team->len >= 2;
}

/* First sentence of an assistant's instructions: enough to describe its
 * specialty to the router. */
static char *
first_sentence (const char *text)
{
  const char *end = text;
  while (*end && *end != '.' && *end != '\n')
    end++;
  g_autofree char *s = g_strndup (text, end - text);
  if (g_utf8_strlen (s, -1) > 160)
    {
      g_autofree char *cut = g_utf8_substring (s, 0, 160);
      return g_strdup (cut);
    }
  return g_steal_pointer (&s);
}

/* Members the user addressed by name, in the order they were mentioned. */
static GArray *
mentioned_members (Conversation *c, const char *text)
{
  GArray *found = g_array_new (FALSE, FALSE, sizeof (int));
  g_autoptr (GArray) pos = g_array_new (FALSE, FALSE, sizeof (glong));
  g_autofree char *hay = g_utf8_casefold (text, -1);

  for (guint i = 0; i < c->team->len; i++)
    {
      TeamMember *t = c->team->pdata[i];
      g_autofree char *full = g_utf8_casefold (t->name, -1);
      const char *at = strstr (hay, full);
      if (!at)
        {
          /* "Entrenador personal" can be called just "entrenador". */
          g_auto (GStrv) words = g_strsplit (full, " ", 2);
          if (words[0] && g_utf8_strlen (words[0], -1) >= 5)
            {
              gboolean unique = TRUE;
              for (guint j = 0; j < c->team->len && unique; j++)
                {
                  g_autofree char *other = g_utf8_casefold (((TeamMember *) c->team->pdata[j])->name, -1);
                  unique = j == i || !g_str_has_prefix (other, words[0]);
                }
              if (unique)
                at = strstr (hay, words[0]);
            }
        }
      if (!at)
        continue;
      glong p = at - hay;
      guint k = 0;
      while (k < pos->len && g_array_index (pos, glong, k) < p)
        k++;
      g_array_insert_val (pos, k, p);
      int idx = (int) i;
      g_array_insert_val (found, k, idx);
    }
  return found;
}

/* Models in a team sometimes start with their own "[Name]:" tag, copying the
 * format they see for their teammates. Returns the text without it. */
static const char *
skip_speaker_tag (const char *text)
{
  if (*text != '[')
    return text;
  const char *close = strchr (text, ']');
  if (!close || close - text > 48 || close[1] != ':')
    return text;
  const char *rest = close + 2;
  while (*rest == ' ')
    rest++;
  return rest;
}

/* A member with nothing to add replies "[PASS]" and the turn moves on. */
static gboolean
is_pass_reply (const char *text)
{
  return g_str_has_prefix (text, "[PASS]");
}

/* While "[PASS]" is still being typed, do not flash it on screen. */
static gboolean
could_become_pass (const char *text)
{
  return *text && !is_pass_reply (text) ? g_str_has_prefix ("[PASS]", text) : is_pass_reply (text);
}

static void
reply_set_member (Reply *r, int member)
{
  TeamMember *t = r->conv->team->pdata[member];
  r->member = member;
  gtk_label_set_text (GTK_LABEL (r->avatar), t->emoji && *t->emoji ? t->emoji : "✦");
  gtk_label_set_text (GTK_LABEL (r->title), t->name);
}

/* ---- chat request ----------------------------------------------------- */

static void
finish_reply (const char *error)
{
  Reply *r = A.reply;
  if (!r)
    return;
  A.reply = NULL;
  A.waiting_for_model = FALSE;
  g_clear_handle_id (&A.render_id, g_source_remove);

  g_autoptr (GString) think_buf = g_string_new (NULL);
  g_autofree char *raw_answer = split_thinking (r, think_buf);
  g_autofree char *think = g_strstrip (g_strdup (think_buf->str));
  const char *answer = r->member >= 0 ? skip_speaker_tag (raw_answer) : raw_answer;
  gboolean passed = r->member >= 0 && is_pass_reply (answer);

  int tokens = A.usage_tokens > 0 ? A.usage_tokens : A.n_chunks;
  g_autoptr (GString) stats = g_string_new (NULL);
  if (A.t_first > 0 && tokens > 0)
    {
      double ttft = (A.t_first - A.t_start) / (double) G_USEC_PER_SEC;
      double decode = (g_get_monotonic_time () - A.t_first) / (double) G_USEC_PER_SEC;
      if (tokens > 1 && decode > 0)
        g_string_append_printf (stats, "⚡ %.1f tok/s   ·   ", (tokens - 1) / decode);
      if (A.search_secs > 0 && r->member <= 0)
        g_string_append_printf (stats, "%s %.1f s   ·   ", TR ("búsqueda", "search"), A.search_secs);
      g_string_append_printf (stats, "%s %.2f s   ·   %d tokens",
                              TR ("primer token", "first token"), ttft, tokens);
    }

  Conversation *c = r->conv;
  if (passed)
    {
      /* This member has nothing to add: drop its bubble, the next one goes on. */
      if (gtk_widget_get_parent (r->root))
        gtk_box_remove (GTK_BOX (A.messages), r->root);
    }
  else
    {
      reply_show (r, answer, think, FALSE);
      reply_finish_footer (r, answer, stats->str, error);
      if (c && *answer)
        {
          store_load_messages (c);
          conversation_add (c, "assistant", answer, think, stats->str);
          if (r->member >= 0)
            {
              TeamMember *t = c->team->pdata[r->member];
              conversation_set_author (c, t->name, t->emoji);
              A.team_answered = TRUE;
            }
          for (guint i = 0; i < r->steps->len; i++)
            {
              ToolStep *st = r->steps->pdata[i];
              if (st->done)
                conversation_add_tool (c, st->name, st->args, st->result, st->ok);
            }
          store_save (c);
          if (c != A.conv)
            store_unload_messages (c);
        }
    }

  gboolean rerender = r->detached && c && c == A.conv;
  /* Team turn: the next member answers, unless this one failed or was stopped. */
  gboolean next = !error && !A.quitting && !r->detached && c && conv_is_team (c) && A.queue->len > 0;
  if (!next)
    {
      g_array_set_size (A.queue, 0);
      if (passed && !A.team_answered && !A.quitting)
        toast ("%s", TR ("Ningún especialista tenía algo que añadir. Nombra a uno para hablarle directo.",
                         "No specialist had anything to add. Name one to talk to them directly."));
    }
  reply_free (r);
  g_clear_object (&A.cancel);

  if (A.quitting)
    return;
  if (next)
    {
      A.reply = reply_new (A.model, NULL, NULL);
      A.reply->conv = c;
      gtk_box_append (GTK_BOX (A.messages), A.reply->root);
      A.cancel = TRACK (g_cancellable_new ());
      scroll_to_bottom ();
      answer_next_in_queue ();
      return;
    }
  if (rerender)
    render_conversation ();
  refresh_chat_list ();
  update_send_button ();
  arm_idle_timer ();
  gtk_widget_grab_focus (GTK_WIDGET (A.input));

  if (A.power_reload_pending)
    {
      A.power_reload_pending = FALSE;
      reload_for_power ();
    }
}

static gboolean
handle_sse_line (const char *line)
{
  if (A.http_status != SOUP_STATUS_OK)
    {
      if (A.http_error->len)
        g_string_append_c (A.http_error, ' ');
      g_string_append (A.http_error, line);
      return FALSE;
    }

  if (!g_str_has_prefix (line, "data:"))
    return FALSE;

  const char *payload = line + 5;
  while (*payload == ' ')
    payload++;
  if (g_str_has_prefix (payload, "[DONE]"))
    return TRUE;

  g_autoptr (JsonParser) parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, payload, -1, NULL))
    return FALSE;
  JsonNode *root = json_parser_get_root (parser);
  if (!JSON_NODE_HOLDS_OBJECT (root))
    return FALSE;
  JsonObject *obj = json_node_get_object (root);

  if (json_object_has_member (obj, "error"))
    {
      JsonNode *e = json_object_get_member (obj, "error");
      const char *msg = NULL;
      if (JSON_NODE_HOLDS_OBJECT (e))
        msg = json_object_get_string_member_with_default (json_node_get_object (e), "message", NULL);
      else if (JSON_NODE_HOLDS_VALUE (e) && json_node_get_value_type (e) == G_TYPE_STRING)
        msg = json_node_get_string (e);
      g_string_assign (A.http_error, msg ? msg : TR ("Error del servidor", "Server error"));
      return TRUE;
    }

  JsonNode *usage = json_object_get_member (obj, "usage");
  if (usage && JSON_NODE_HOLDS_OBJECT (usage))
    A.usage_tokens = json_object_get_int_member_with_default (json_node_get_object (usage),
                                                              "completion_tokens", A.usage_tokens);

  JsonNode *choices = json_object_get_member (obj, "choices");
  if (!choices || !JSON_NODE_HOLDS_ARRAY (choices) || json_array_get_length (json_node_get_array (choices)) == 0)
    return FALSE;
  JsonNode *choice = json_array_get_element (json_node_get_array (choices), 0);
  if (!JSON_NODE_HOLDS_OBJECT (choice))
    return FALSE;
  JsonNode *delta_node = json_object_get_member (json_node_get_object (choice), "delta");
  if (!delta_node || !JSON_NODE_HOLDS_OBJECT (delta_node))
    return FALSE;
  JsonObject *delta = json_node_get_object (delta_node);

  const char *content = json_object_get_string_member_with_default (delta, "content", NULL);
  const char *reasoning = json_object_get_string_member_with_default (delta, "reasoning_content", NULL);
  if (!reasoning)
    reasoning = json_object_get_string_member_with_default (delta, "reasoning", NULL);

  /* Tool calls arrive as deltas keyed by index; flm sends each whole, but
   * the OpenAI format allows the arguments to be split across chunks. */
  JsonNode *tool_calls = json_object_get_member (delta, "tool_calls");
  if (tool_calls && JSON_NODE_HOLDS_ARRAY (tool_calls))
    {
      JsonArray *arr = json_node_get_array (tool_calls);
      for (guint i = 0; i < json_array_get_length (arr); i++)
        {
          JsonObject *tc = json_array_get_object_element (arr, i);
          if (!tc)
            continue;
          guint index = (guint) json_object_get_int_member_with_default (tc, "index", i);
          if (index > 8)
            continue;
          while (A.calls->len <= index)
            {
              PendingCall *pc = g_new0 (PendingCall, 1);
              pc->args = g_string_new (NULL);
              g_ptr_array_add (A.calls, pc);
            }
          PendingCall *pc = A.calls->pdata[index];
          const char *id = json_object_get_string_member_with_default (tc, "id", NULL);
          if (id && *id && !pc->id)
            pc->id = g_strdup (id);
          JsonNode *fn_node = json_object_get_member (tc, "function");
          if (fn_node && JSON_NODE_HOLDS_OBJECT (fn_node))
            {
              JsonObject *fn = json_node_get_object (fn_node);
              const char *fname = json_object_get_string_member_with_default (fn, "name", NULL);
              if (fname && *fname && !pc->name)
                pc->name = g_strdup (fname);
              JsonNode *an = json_object_get_member (fn, "arguments");
              if (an && JSON_NODE_HOLDS_VALUE (an) && json_node_get_value_type (an) == G_TYPE_STRING)
                g_string_append (pc->args, json_node_get_string (an));
              else if (an && JSON_NODE_HOLDS_OBJECT (an))
                {
                  g_autofree char *as_text = json_to_string (an, FALSE);
                  g_string_append (pc->args, as_text);
                }
            }
        }
    }

  gboolean got = FALSE;
  if (content && *content)
    {
      g_string_append (A.reply->text, content);
      got = TRUE;
    }
  if (reasoning && *reasoning)
    {
      g_string_append (A.reply->reasoning, reasoning);
      got = TRUE;
    }
  if (got)
    {
      A.n_chunks++;
      if (!A.t_first)
        A.t_first = g_get_monotonic_time ();
      queue_render ();
    }
  return FALSE;
}

static void read_next_line (GDataInputStream *ds);

static void
on_sse_line (GObject *src, GAsyncResult *res, gpointer user_data)
{
  (void) user_data;
  GDataInputStream *ds = G_DATA_INPUT_STREAM (src);
  g_autoptr (GError) error = NULL;
  g_autofree char *line = g_data_input_stream_read_line_finish_utf8 (ds, res, NULL, &error);

  if (error || !A.reply)
    {
      g_object_unref (ds);
      if (!error)
        return;
      finish_reply (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)
                      ? TR ("Detenido", "Stopped") : error->message);
      return;
    }

  if (line ? handle_sse_line (g_strchomp (line)) : TRUE)
    {
      g_object_unref (ds);
      if (A.http_status != SOUP_STATUS_OK)
        {
          g_autofree char *msg = g_strdup_printf ("HTTP %u %s", A.http_status, A.http_error->str);
          finish_reply (msg);
        }
      else if (!A.http_error->len && A.calls->len > 0)
        run_tool_round ();
      else
        finish_reply (A.http_error->len ? A.http_error->str : NULL);
      return;
    }

  read_next_line (ds);
}

static void
read_next_line (GDataInputStream *ds)
{
  g_data_input_stream_read_line_async (ds, G_PRIORITY_DEFAULT, A.cancel, on_sse_line, NULL);
}

static void
on_chat_sent (GObject *src, GAsyncResult *res, gpointer user_data)
{
  SoupMessage *msg = user_data;
  g_autoptr (GError) error = NULL;
  GInputStream *in = soup_session_send_finish (SOUP_SESSION (src), res, &error);

  A.http_status = soup_message_get_status (msg);
  g_object_unref (msg);

  if (!in)
    {
      finish_reply (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)
                      ? TR ("Detenido", "Stopped") : error->message);
      return;
    }

  GDataInputStream *ds = TRACK (g_data_input_stream_new (in));
  g_object_unref (in);
  read_next_line (ds);
}

/* ---- skills ----------------------------------------------------------- */

static gboolean
skill_enabled_cb (const char *id, gpointer data)
{
  (void) data;
  return g_hash_table_contains (A.skills_on, id);
}

/* Not every model can call tools: flm labels the ones that can. */
static gboolean
model_supports_tools (void)
{
  FlmModel *m = installed_find (A.model);
  return m && m->labels && g_strv_contains ((const char *const *) m->labels, "tool-calling");
}

static gboolean
tools_active (const Reply *r)
{
  return !r->no_tools && g_hash_table_size (A.skills_on) > 0 && model_supports_tools ();
}

static void
pending_call_free (gpointer p)
{
  PendingCall *pc = p;
  g_free (pc->id);
  g_free (pc->name);
  g_string_free (pc->args, TRUE);
  g_free (pc);
}

static void run_next_step (void);

static void
on_skill_done (char *result, gboolean ok, gpointer data)
{
  ToolCtx *ctx = data;
  guint serial = ctx->serial, index = ctx->index;
  g_free (ctx);
  g_autofree char *text = result;

  /* The reply may be gone (stopped, chat closed) by the time a slow skill answers. */
  if (A.quitting || !A.reply || A.reply->serial != serial)
    return;
  if (g_cancellable_is_cancelled (A.cancel))
    {
      finish_reply (TR ("Detenido", "Stopped"));
      return;
    }

  ToolStep *st = A.reply->steps->pdata[index];
  if (ok && (g_str_equal (st->name, "calendar_add") || g_str_equal (st->name, "calendar_change") ||
             g_str_equal (st->name, "calendar_delete") || g_str_equal (st->name, "calendar_undo") ||
             g_str_equal (st->name, "calendar_done")))
    {
      calendar_ui_refresh ();
      calendar_alerts_reschedule ();
    }
  st->result = (g_utf8_strlen (text, -1) > 1500) ? g_utf8_substring (text, 0, 1500) : g_strdup (text);
  st->ok = ok;
  st->done = TRUE;
  tool_label_update (st->label, st->name, st->args, st->result, st->ok, FALSE);
  A.reply->next_step = index + 1;
  run_next_step ();
}

/* Runs the next requested skill, or goes back to the model with the results. */
static void
run_next_step (void)
{
  Reply *r = A.reply;
  if (!r)
    return;

  if (r->next_step < r->steps->len)
    {
      ToolStep *st = r->steps->pdata[r->next_step];
      g_autofree char *status = g_strdup_printf (TR ("Usando %s…", "Using %s…"), skill_title (st->name));
      gtk_label_set_text (GTK_LABEL (r->stats), status);
      gtk_widget_set_visible (r->copy, FALSE);
      gtk_widget_set_visible (r->footer, TRUE);

      ToolCtx *ctx = g_new0 (ToolCtx, 1);
      ctx->serial = r->serial;
      ctx->index = r->next_step;
      skill_run (st->name, st->args, A.cancel, on_skill_done, ctx);
      return;
    }

  gtk_widget_set_visible (r->footer, FALSE);
  if (r->rounds >= MAX_TOOL_ROUNDS)
    r->no_tools = TRUE; /* stop offering tools so the model has to answer */
  start_request ();
}

/* The model asked for skills: run them, then ask again with the results. */
static void
run_tool_round (void)
{
  Reply *r = A.reply;
  r->rounds++;

  /* Whatever it wrote before asking is not the answer. */
  g_string_truncate (r->text, 0);
  g_string_truncate (r->reasoning, 0);
  render_reply (FALSE);

  guint first = r->steps->len;
  for (guint i = 0; i < A.calls->len; i++)
    {
      PendingCall *pc = A.calls->pdata[i];
      if (!pc->name)
        continue;
      ToolStep *st = g_new0 (ToolStep, 1);
      st->call_id = pc->id ? g_strdup (pc->id) : g_strdup_printf ("call_%u_%u", r->serial, r->steps->len + 1);
      st->name = g_strdup (pc->name);
      st->args = g_strdup (pc->args->len ? pc->args->str : "{}");
      st->label = reply_tool_row (r, st->name, st->args, NULL, TRUE, TRUE);
      g_ptr_array_add (r->steps, st);
    }
  g_ptr_array_set_size (A.calls, 0);

  if (r->steps->len == first)
    {
      /* A call with no name is useless: answer without tools. */
      r->no_tools = TRUE;
      start_request ();
      return;
    }
  r->next_step = first;
  run_next_step ();
}

/* The question plus the passages found for it, in the format the model is
 * told to cite from. */
static char *
build_grounded_prompt (const StoreMsg *m)
{
  GString *s = g_string_new (NULL);
  if (m->sources->len == 0)
    {
      g_string_append_printf (s, "The user has documents attached, but none of their passages matched this message. "
                                 "If the message is about their documents, say you could not find it there; "
                                 "otherwise just answer normally.\n\nMessage: %s", m->content);
      return g_string_free (s, FALSE);
    }
  g_string_append (s, "Answer using only the numbered sources below, and cite them like [1] or [2]. "
                      "If the sources do not contain the answer, say you could not find it in the user's documents.\n\n"
                      "Sources:\n");
  for (guint i = 0; i < m->sources->len; i++)
    {
      StoreSource *src = m->sources->pdata[i];
      if (src->page > 0)
        g_string_append_printf (s, "[%u] (%s, p. %d)\n%s\n\n", i + 1, src->file, src->page, src->text);
      else
        g_string_append_printf (s, "[%u] (%s)\n%s\n\n", i + 1, src->file, src->text);
    }
  g_string_append_printf (s, "Question: %s", m->content);
  return g_string_free (s, FALSE);
}

static void
start_request (void)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "model");
  json_builder_add_string_value (b, A.model);
  json_builder_set_member_name (b, "stream");
  json_builder_add_boolean_value (b, TRUE);
  json_builder_set_member_name (b, "messages");
  json_builder_begin_array (b);
  /* The assistant's instructions, plus the short-replies hint: decode speed
   * is fixed by memory bandwidth, so shorter answers are the biggest win.
   * Both stay constant within a chat so flm can reuse its prompt cache. */
  Conversation *conv = A.reply->conv;
  TeamMember *me = A.reply->member >= 0 ? conv->team->pdata[A.reply->member] : NULL;
  g_autoptr (GString) system = g_string_new (me ? me->instructions : conv->system);
  if (me)
    {
      g_string_append_printf (system, "\n\nYou are %s, one of several specialists in this chat. The team:", me->name);
      for (guint i = 0; i < conv->team->len; i++)
        {
          TeamMember *t = conv->team->pdata[i];
          g_autofree char *role = first_sentence (t->instructions);
          g_string_append_printf (system, "\n- %s: %s", t->name, role);
        }
      g_string_append (system, "\nBuild on what teammates already said and never repeat it. Only cover what belongs "
                               "to your specialty. If nothing in the user's message needs your specialty, or a "
                               "teammate already covered it, reply with exactly [PASS] and nothing else. "
                               "Teammates' messages appear as \"[Name]: text\"; never start your own message "
                               "with a name tag.");
    }
  if (A.concise)
    {
      if (system->len)
        g_string_append (system, "\n\n");
      g_string_append (system, "Be concise and direct: answer in a few sentences or a short list, "
                               "unless the user asks for more detail. Reply in the same language "
                               "the user writes in.");
    }
  /* Skills: a standing instruction that makes the model reach for the tools. */
  if (tools_active (A.reply))
    {
      g_autofree char *hint = skills_instructions (skill_enabled_cb, NULL);
      if (hint)
        g_string_append_printf (system, "%s%s", system->len ? "\n\n" : "", hint);
    }
  if (system->len)
    {
      json_builder_begin_object (b);
      json_builder_set_member_name (b, "role");
      json_builder_add_string_value (b, "system");
      json_builder_set_member_name (b, "content");
      json_builder_add_string_value (b, system->str);
      json_builder_end_object (b);
    }
  guint last_user = G_MAXUINT;
  for (guint i = 0; i < conv->msgs->len; i++)
    if (g_str_equal (((StoreMsg *) conv->msgs->pdata[i])->role, "user"))
      last_user = i;

  for (guint i = 0; i < conv->msgs->len; i++)
    {
      StoreMsg *m = conv->msgs->pdata[i];
      /* In a team, other members' replies are shown to this one as tagged
       * messages, so it knows who said what. */
      gboolean other = me && m->author && !g_str_equal (m->author, me->name);
      g_autofree char *tagged = other ? g_strdup_printf ("[%s]: %s", m->author, m->content) : NULL;
      /* Only the newest question carries passages: older turns already live
       * in the answers, and a short history keeps prefill (and flm's cache) fast. */
      g_autofree char *grounded = i == last_user && m->sources ? build_grounded_prompt (m) : NULL;
      if (grounded)
        tagged = g_steal_pointer (&grounded);

      /* The date and time stored with the message. Saved rather than recomputed,
       * so the history is the same every turn and flm's cache keeps working:
       * recomputing it made every turn without tools about 2 s slower. */
      if (m->note && g_str_equal (m->role, "user"))
        {
          g_autofree char *base = tagged ? g_steal_pointer (&tagged) : g_strdup (m->content);
          tagged = g_strdup_printf ("%s\n\n%s", base, m->note);
        }
      /* Earlier replies keep their tool calls in the history. Without them the
       * model sees a run of answers that never used a tool and copies that:
       * measured on qwen3.5:9b, 0/16 calls after three such turns, against
       * 16/16 when the calls are replayed. */
      if (!other && m->tools && m->tools->len > 0 && g_str_equal (m->role, "assistant") && tools_active (A.reply))
        {
          json_builder_begin_object (b);
          json_builder_set_member_name (b, "role");
          json_builder_add_string_value (b, "assistant");
          json_builder_set_member_name (b, "content");
          json_builder_add_string_value (b, "");
          json_builder_set_member_name (b, "tool_calls");
          json_builder_begin_array (b);
          for (guint k = 0; k < m->tools->len; k++)
            {
              StoreTool *t = m->tools->pdata[k];
              g_autofree char *call_id = g_strdup_printf ("call_h%u_%u", i, k);
              json_builder_begin_object (b);
              json_builder_set_member_name (b, "id");
              json_builder_add_string_value (b, call_id);
              json_builder_set_member_name (b, "type");
              json_builder_add_string_value (b, "function");
              json_builder_set_member_name (b, "function");
              json_builder_begin_object (b);
              json_builder_set_member_name (b, "name");
              json_builder_add_string_value (b, t->name);
              json_builder_set_member_name (b, "arguments");
              json_builder_add_string_value (b, t->args);
              json_builder_end_object (b);
              json_builder_end_object (b);
            }
          json_builder_end_array (b);
          json_builder_end_object (b);
          for (guint k = 0; k < m->tools->len; k++)
            {
              StoreTool *t = m->tools->pdata[k];
              g_autofree char *call_id = g_strdup_printf ("call_h%u_%u", i, k);
              /* A reading of the laptop is stale by the next question: say so,
               * or the model reuses it. Other results are kept, shortened. */
              g_autofree char *result = g_str_equal (t->name, "system_status")
                ? g_strdup ("(outdated: this value changes, call the tool again if you need it)")
                : (g_utf8_strlen (t->result, -1) > 300 ? g_utf8_substring (t->result, 0, 300) : g_strdup (t->result));
              json_builder_begin_object (b);
              json_builder_set_member_name (b, "role");
              json_builder_add_string_value (b, "tool");
              json_builder_set_member_name (b, "tool_call_id");
              json_builder_add_string_value (b, call_id);
              json_builder_set_member_name (b, "content");
              json_builder_add_string_value (b, result);
              json_builder_end_object (b);
            }
        }

      json_builder_begin_object (b);
      json_builder_set_member_name (b, "role");
      json_builder_add_string_value (b, other ? "user" : m->role);
      json_builder_set_member_name (b, "content");
      json_builder_add_string_value (b, tagged ? tagged : m->content);
      json_builder_end_object (b);
    }
  /* This turn's tool calls and what they returned, so the model can use them. */
  Reply *rep_ = A.reply;
  guint answered = 0;
  for (guint i = 0; i < rep_->steps->len; i++)
    answered += ((ToolStep *) rep_->steps->pdata[i])->done;
  if (answered > 0)
    {
      json_builder_begin_object (b);
      json_builder_set_member_name (b, "role");
      json_builder_add_string_value (b, "assistant");
      json_builder_set_member_name (b, "content");
      json_builder_add_string_value (b, "");
      json_builder_set_member_name (b, "tool_calls");
      json_builder_begin_array (b);
      for (guint i = 0; i < rep_->steps->len; i++)
        {
          ToolStep *st = rep_->steps->pdata[i];
          if (!st->done)
            continue;
          json_builder_begin_object (b);
          json_builder_set_member_name (b, "id");
          json_builder_add_string_value (b, st->call_id);
          json_builder_set_member_name (b, "type");
          json_builder_add_string_value (b, "function");
          json_builder_set_member_name (b, "function");
          json_builder_begin_object (b);
          json_builder_set_member_name (b, "name");
          json_builder_add_string_value (b, st->name);
          json_builder_set_member_name (b, "arguments");
          json_builder_add_string_value (b, st->args);
          json_builder_end_object (b);
          json_builder_end_object (b);
        }
      json_builder_end_array (b);
      json_builder_end_object (b);
      for (guint i = 0; i < rep_->steps->len; i++)
        {
          ToolStep *st = rep_->steps->pdata[i];
          if (!st->done)
            continue;
          json_builder_begin_object (b);
          json_builder_set_member_name (b, "role");
          json_builder_add_string_value (b, "tool");
          json_builder_set_member_name (b, "tool_call_id");
          json_builder_add_string_value (b, st->call_id);
          json_builder_set_member_name (b, "content");
          json_builder_add_string_value (b, st->result);
          json_builder_end_object (b);
        }
    }
  json_builder_end_array (b);
  if (tools_active (rep_))
    skills_build_tools (b, skill_enabled_cb, NULL);
  json_builder_end_object (b);

  g_autoptr (JsonNode) root = json_builder_get_root (b);
  char *body = json_to_string (root, FALSE);
  g_autoptr (GBytes) bytes = g_bytes_new_take (body, strlen (body));
  g_autofree char *url = flm_url ("/v1/chat/completions");
  SoupMessage *msg = TRACK (soup_message_new ("POST", url));
  soup_message_set_request_body_from_bytes (msg, "application/json", bytes);

  g_ptr_array_set_size (A.calls, 0);
  A.t_start = g_get_monotonic_time ();
  A.t_first = 0;
  A.n_chunks = 0;
  A.usage_tokens = 0;
  A.http_status = 0;
  g_string_truncate (A.http_error, 0);

  soup_session_send_async (flm_session (), msg, G_PRIORITY_DEFAULT, A.cancel, on_chat_sent, msg);
}

/* Picks the next team member from the queue and starts its reply. */
static void
answer_next_in_queue (void)
{
  int member = g_array_index (A.queue, int, 0);
  g_array_remove_index (A.queue, 0);
  reply_set_member (A.reply, member);
  start_request ();
}

static void
on_routed (GObject *src, GAsyncResult *res, gpointer user_data)
{
  (void) user_data;
  g_autoptr (GError) error = NULL;
  SoupMessage *msg = soup_session_get_async_result_message (SOUP_SESSION (src), res);
  g_autoptr (GBytes) body = soup_session_send_and_read_finish (SOUP_SESSION (src), res, &error);

  if (!A.reply || A.quitting)
    return;
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      finish_reply (TR ("Detenido", "Stopped"));
      return;
    }

  Conversation *c = A.reply->conv;
  g_autofree char *answer = NULL;
  if (body && soup_message_get_status (msg) == SOUP_STATUS_OK)
    {
      g_autoptr (JsonParser) parser = json_parser_new ();
      if (json_parser_load_from_data (parser, g_bytes_get_data (body, NULL), g_bytes_get_size (body), NULL))
        {
          JsonNode *root = json_parser_get_root (parser);
          JsonObject *o = JSON_NODE_HOLDS_OBJECT (root) ? json_node_get_object (root) : NULL;
          JsonNode *ch = o ? json_object_get_member (o, "choices") : NULL;
          if (ch && JSON_NODE_HOLDS_ARRAY (ch) && json_array_get_length (json_node_get_array (ch)) > 0)
            {
              JsonObject *c0 = json_array_get_object_element (json_node_get_array (ch), 0);
              JsonNode *m = c0 ? json_object_get_member (c0, "message") : NULL;
              if (m && JSON_NODE_HOLDS_OBJECT (m))
                answer = g_strdup (json_object_get_string_member_with_default (json_node_get_object (m), "content", ""));
            }
        }
    }

  /* Read member numbers in order, skipping any reasoning block. */
  g_array_set_size (A.queue, 0);
  const char *p = answer ? answer : "";
  const char *think_end = strstr (p, "</think>");
  if (think_end)
    p = think_end + strlen ("</think>");
  for (; *p; p++)
    if (g_ascii_isdigit (*p))
      {
        int n = (int) strtol (p, (char **) &p, 10) - 1;
        gboolean dup = FALSE;
        for (guint i = 0; i < A.queue->len; i++)
          dup |= g_array_index (A.queue, int, i) == n;
        if (n >= 0 && n < (int) c->team->len && !dup)
          g_array_append_val (A.queue, n);
        if (!*p)
          break;
      }

  if (A.queue->len == 0)
    {
      /* Router unsure: the member who spoke last continues, else the first. */
      int n = 0;
      for (guint i = c->msgs->len; i-- > 0;)
        {
          StoreMsg *m = c->msgs->pdata[i];
          if (!m->author)
            continue;
          for (guint j = 0; j < c->team->len; j++)
            if (g_str_equal (((TeamMember *) c->team->pdata[j])->name, m->author))
              n = (int) j;
          break;
        }
      g_array_append_val (A.queue, n);
    }
  answer_next_in_queue ();
}

/* Decides which team members answer: the ones the user named, otherwise a
 * short, non-streamed question to the model. */
static void
route_team_turn (void)
{
  Conversation *c = A.reply->conv;
  StoreMsg *last = c->msgs->pdata[c->msgs->len - 1];

  g_autoptr (GArray) named = mentioned_members (c, last->content);
  if (named->len > 0)
    {
      g_array_set_size (A.queue, 0);
      g_array_append_vals (A.queue, named->data, named->len);
      answer_next_in_queue ();
      return;
    }

  g_autoptr (GString) sys = g_string_new ("You decide which team members should answer the user's last message. Team:");
  for (guint i = 0; i < c->team->len; i++)
    {
      TeamMember *t = c->team->pdata[i];
      g_autofree char *role = first_sentence (t->instructions);
      g_string_append_printf (sys, "\n%u. %s: %s", i + 1, t->name, role);
    }
  g_string_append (sys, "\nReply with only the member numbers, separated by commas. Pick the single best member "
                        "when the message fits one specialty; list several only if the message clearly mixes "
                        "different specialties, one number per specialty needed, most relevant first.");

  g_autoptr (GString) question = g_string_new (NULL);
  for (guint i = c->msgs->len - 1; i-- > 0;)
    {
      StoreMsg *m = c->msgs->pdata[i];
      if (m->author)
        {
          g_string_append_printf (question, "(The previous reply was from %s.)\n", m->author);
          break;
        }
    }
  g_string_append_printf (question, "User's last message: %s", last->content);

  g_autoptr (JsonBuilder) b = json_builder_new ();
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "model");
  json_builder_add_string_value (b, A.model);
  json_builder_set_member_name (b, "stream");
  json_builder_add_boolean_value (b, FALSE);
  json_builder_set_member_name (b, "max_tokens");
  json_builder_add_int_value (b, 12);
  json_builder_set_member_name (b, "temperature");
  json_builder_add_double_value (b, 0);
  json_builder_set_member_name (b, "messages");
  json_builder_begin_array (b);
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "role");
  json_builder_add_string_value (b, "system");
  json_builder_set_member_name (b, "content");
  json_builder_add_string_value (b, sys->str);
  json_builder_end_object (b);
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "role");
  json_builder_add_string_value (b, "user");
  json_builder_set_member_name (b, "content");
  json_builder_add_string_value (b, question->str);
  json_builder_end_object (b);
  json_builder_end_array (b);
  json_builder_end_object (b);

  g_autoptr (JsonNode) root = json_builder_get_root (b);
  char *body = json_to_string (root, FALSE);
  g_autoptr (GBytes) bytes = g_bytes_new_take (body, strlen (body));
  g_autofree char *url = flm_url ("/v1/chat/completions");
  g_autoptr (SoupMessage) msg = TRACK (soup_message_new ("POST", url));
  soup_message_set_request_body_from_bytes (msg, "application/json", bytes);
  soup_session_send_and_read_async (flm_session (), msg, G_PRIORITY_DEFAULT, A.cancel, on_routed, NULL);
}

/* ---- documents -------------------------------------------------------- */

static void
add_lib_by_id (GPtrArray *out, const char *id)
{
  RagLibrary *lib = rag_library_find (A.libraries, id);
  if (lib && lib->docs->len > 0 && !lib->busy && !g_ptr_array_find (out, lib, NULL))
    g_ptr_array_add (out, lib);
}

static void
add_assistant_libs (GPtrArray *out, const char *assistant_id)
{
  Assistant *as = assistants_find (A.assistants, assistant_id);
  for (guint i = 0; as && as->libs && i < as->libs->len; i++)
    add_lib_by_id (out, as->libs->pdata[i]);
}

/* Libraries the open chat searches: the ones attached to it, plus those of
 * its assistant (or of every member of a team). The elements are not owned. */
static GPtrArray *
effective_libs (void)
{
  GPtrArray *out = g_ptr_array_new ();
  if (!A.conv)
    return out;
  for (guint i = 0; i < A.conv->libs->len; i++)
    add_lib_by_id (out, A.conv->libs->pdata[i]);

  if (A.conv_saved)
    {
      if (A.conv->team->len >= 2)
        for (guint i = 0; i < A.conv->team->len; i++)
          add_assistant_libs (out, ((TeamMember *) A.conv->team->pdata[i])->id);
      else
        add_assistant_libs (out, A.conv->assistant_id);
    }
  else if (A.team_mode)
    for (guint i = 0; i < A.pick_team->len; i++)
      add_assistant_libs (out, A.pick_team->pdata[i]);
  else
    add_assistant_libs (out, A.pick_id);
  return out;
}

static RagIndex *
index_for (GPtrArray *libs)
{
  g_autoptr (GString) key = g_string_new (NULL);
  for (guint i = 0; i < libs->len; i++)
    {
      RagLibrary *lib = libs->pdata[i];
      g_string_append_printf (key, "%s:%u:%u;", lib->id, lib->docs->len, rag_library_chunk_count (lib));
    }
  if (!A.rag_index || g_strcmp0 (A.rag_key, key->str) != 0)
    {
      drop_rag_index ();
      A.rag_index = rag_index_new (libs);
      A.rag_key = g_strdup (key->str);
    }
  return A.rag_index;
}

/* After retrieval (or without it): route a team turn, or answer. */
static void
continue_reply (void)
{
  if (conv_is_team (A.reply->conv) && A.reply->member < 0)
    route_team_turn ();
  else
    start_request ();
}

typedef struct {
  char *question;
} ExpandCtx;

/* Keywords from the model, with any reasoning block removed. */
static char *
parse_keywords (GBytes *body)
{
  g_autoptr (JsonParser) parser = json_parser_new ();
  if (!body || !json_parser_load_from_data (parser, g_bytes_get_data (body, NULL), g_bytes_get_size (body), NULL))
    return g_strdup ("");
  JsonNode *root = json_parser_get_root (parser);
  JsonObject *o = JSON_NODE_HOLDS_OBJECT (root) ? json_node_get_object (root) : NULL;
  JsonNode *ch = o ? json_object_get_member (o, "choices") : NULL;
  if (!ch || !JSON_NODE_HOLDS_ARRAY (ch) || json_array_get_length (json_node_get_array (ch)) == 0)
    return g_strdup ("");
  JsonObject *c0 = json_array_get_object_element (json_node_get_array (ch), 0);
  JsonNode *m = c0 ? json_object_get_member (c0, "message") : NULL;
  if (!m || !JSON_NODE_HOLDS_OBJECT (m))
    return g_strdup ("");
  const char *text = json_object_get_string_member_with_default (json_node_get_object (m), "content", "");
  const char *end = strstr (text, "</think>");
  if (end)
    text = end + strlen ("</think>");
  g_autofree char *kw = g_strdup (text);
  if (strlen (kw) > 300)
    kw[300] = '\0';
  return g_strstrip (g_steal_pointer (&kw));
}

static void
on_expanded (GObject *src, GAsyncResult *res, gpointer user_data)
{
  ExpandCtx *ctx = user_data;
  g_autoptr (GError) error = NULL;
  SoupMessage *msg = soup_session_get_async_result_message (SOUP_SESSION (src), res);
  g_autoptr (GBytes) body = soup_session_send_and_read_finish (SOUP_SESSION (src), res, &error);
  g_autofree char *question = ctx->question;
  g_free (ctx);

  if (!A.reply || A.quitting)
    return;
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      finish_reply (TR ("Detenido", "Stopped"));
      return;
    }

  /* If expansion failed the plain question is still searched. */
  g_autofree char *keywords = body && soup_message_get_status (msg) == SOUP_STATUS_OK ? parse_keywords (body)
                                                                                      : g_strdup ("");
  g_autofree char *query = g_strdup_printf ("%s %s", question, keywords);

  Conversation *c = A.reply->conv;
  g_autoptr (GPtrArray) libs = effective_libs ();
  g_autoptr (GPtrArray) hits = libs->len ? rag_search (index_for (libs), query, 4) : g_ptr_array_new ();

  conversation_begin_sources (c);
  for (guint i = 0; i < hits->len; i++)
    {
      RagHit *h = hits->pdata[i];
      conversation_add_source (c, h->file, h->page, h->text);
    }
  store_save (c);

  StoreMsg *last = c->msgs->pdata[c->msgs->len - 1];
  reply_set_sources (A.reply, last->sources);
  gtk_widget_set_visible (A.reply->footer, FALSE);
  A.search_secs = (g_get_monotonic_time () - A.search_start) / (double) G_USEC_PER_SEC;
  A.search_start = 0;
  continue_reply ();
}

/* Asks the model for search keywords (synonyms and related terms), which
 * measured far better than searching the bare question: 7/12 -> 12/12. */
static void
begin_retrieval (void)
{
  Conversation *c = A.reply->conv;
  StoreMsg *last = c->msgs->pdata[c->msgs->len - 1];

  A.search_start = g_get_monotonic_time ();
  A.search_secs = 0;
  gtk_label_set_text (GTK_LABEL (A.reply->stats), TR ("Buscando en tus documentos…", "Searching your documents…"));
  gtk_widget_set_visible (A.reply->copy, FALSE);
  gtk_widget_set_visible (A.reply->footer, TRUE);

  /* A short follow-up ("and the second one?") only makes sense with the
   * question before it. */
  g_autofree char *question = g_strdup (last->content);
  g_autofree char *asked = g_strdup (last->content);
  if (g_utf8_strlen (last->content, -1) < 40)
    for (guint i = c->msgs->len - 1; i-- > 0;)
      {
        StoreMsg *prev = c->msgs->pdata[i];
        if (g_str_equal (prev->role, "user"))
          {
            g_free (asked);
            asked = g_strdup_printf ("%s %s", prev->content, last->content);
            break;
          }
      }
  g_autofree char *user_text = g_utf8_strlen (asked, -1) > 400 ? g_utf8_substring (asked, 0, 400) : g_strdup (asked);

  g_autoptr (JsonBuilder) b = json_builder_new ();
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "model");
  json_builder_add_string_value (b, A.model);
  json_builder_set_member_name (b, "stream");
  json_builder_add_boolean_value (b, FALSE);
  json_builder_set_member_name (b, "max_tokens");
  json_builder_add_int_value (b, 60);
  json_builder_set_member_name (b, "temperature");
  json_builder_add_double_value (b, 0);
  json_builder_set_member_name (b, "messages");
  json_builder_begin_array (b);
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "role");
  json_builder_add_string_value (b, "system");
  json_builder_set_member_name (b, "content");
  /* Both languages: people often ask in Spanish about English documents.
   * Measured on a real English PDF: bare question 1/7, same-language
   * keywords 3/7, both languages 6/7. */
  json_builder_add_string_value (b, "You help search the user's documents. Given a question, write 10 search keywords: "
                                    "the important words of the question plus close synonyms and related terms the "
                                    "documents might use instead. Write them both in the language of the question "
                                    "and in English. Reply with the keywords only, separated by spaces.");
  json_builder_end_object (b);
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "role");
  json_builder_add_string_value (b, "user");
  json_builder_set_member_name (b, "content");
  json_builder_add_string_value (b, user_text);
  json_builder_end_object (b);
  json_builder_end_array (b);
  json_builder_end_object (b);

  g_autoptr (JsonNode) root = json_builder_get_root (b);
  char *body = json_to_string (root, FALSE);
  g_autoptr (GBytes) bytes = g_bytes_new_take (body, strlen (body));
  g_autofree char *url = flm_url ("/v1/chat/completions");
  g_autoptr (SoupMessage) msg = TRACK (soup_message_new ("POST", url));
  soup_message_set_request_body_from_bytes (msg, "application/json", bytes);

  ExpandCtx *ctx = g_new0 (ExpandCtx, 1);
  ctx->question = g_steal_pointer (&question);
  soup_session_send_and_read_async (flm_session (), msg, G_PRIORITY_DEFAULT, A.cancel, on_expanded, ctx);
}

/* Starts the reply in A.reply: searches the documents, then routes team
 * turns, then answers. */
static void
begin_reply (void)
{
  g_autoptr (GPtrArray) libs = effective_libs ();
  if (libs->len > 0 && A.reply->member < 0)
    begin_retrieval ();
  else
    continue_reply ();
}

static char *
make_title (const char *text)
{
  const char *nl = strchr (text, '\n');
  g_autofree char *line = nl ? g_strndup (text, nl - text) : g_strdup (text);
  if (g_utf8_strlen (line, -1) <= 48)
    return g_steal_pointer (&line);
  g_autofree char *cut = g_utf8_substring (line, 0, 47);
  return g_strconcat (g_strchomp (cut), "…", NULL);
}

static gboolean
send_text (const char *raw)
{
  g_autofree char *text = g_strstrip (g_strdup (raw));
  if (!*text || is_streaming () || !A.model || !A.flm_present)
    return FALSE;
  if (!memlock_ok ())
    {
      show_memlock_dialog ();
      return FALSE;
    }

  if (!A.conv)
    {
      A.conv = conversation_new ();
      A.conv_saved = FALSE;
    }
  if (!A.conv_saved)
    {
      A.conv->title = make_title (text);
      Assistant *as = assistants_find (A.assistants, A.pick_id);
      if (A.team_mode)
        {
          for (guint i = 0; i < A.pick_team->len; i++)
            {
              Assistant *m = assistants_find (A.assistants, A.pick_team->pdata[i]);
              if (m)
                g_ptr_array_add (A.conv->team, team_member_new (m->id, m->name, m->emoji, m->instructions));
            }
          /* A one-member "team" is just that assistant. */
          as = A.conv->team->len == 1 ? assistants_find (A.assistants, ((TeamMember *) A.conv->team->pdata[0])->id) : NULL;
          if (as)
            g_ptr_array_set_size (A.conv->team, 0);
        }
      if (as)
        {
          A.conv->assistant_id = g_strdup (as->id);
          A.conv->assistant_name = g_strdup (as->name);
          A.conv->assistant_emoji = g_strdup (as->emoji);
          A.conv->system = g_strdup (as->instructions);
        }
      g_ptr_array_insert (A.convs, 0, A.conv);
      A.conv_saved = TRUE;
    }

  g_free (A.conv->model);
  A.conv->model = g_strdup (A.model);
  A.team_answered = FALSE;
  A.search_secs = 0;
  conversation_add (A.conv, "user", text, NULL, NULL);
  if (g_hash_table_contains (A.skills_on, "current_datetime"))
    {
      g_autofree char *note = skill_now_note ();
      conversation_set_note (A.conv, note);
    }
  store_save (A.conv);
  conv_to_front (A.conv);
  refresh_chat_list ();

  if (A.conv->msgs->len == 1)
    clear_messages ();
  gtk_stack_set_visible_child_name (A.chat_stack, "chat");
  add_user_bubble (text);

  if (conv_is_team (A.conv))
    A.reply = reply_new (A.model, "✦", TR ("El equipo está eligiendo…", "The team is choosing…"));
  else
    A.reply = reply_new (A.model, A.conv->assistant_emoji, A.conv->assistant_name);
  A.reply->conv = A.conv;
  gtk_box_append (GTK_BOX (A.messages), A.reply->root);
  A.cancel = TRACK (g_cancellable_new ());
  scroll_to_bottom ();
  disarm_idle ();
  update_send_button ();

  if (flm_state () == FLM_READY)
    begin_reply ();
  else
    {
      A.waiting_for_model = TRUE;
      ensure_loaded ();
    }
  return TRUE;
}

static void
stop_generation (void)
{
  if (!A.reply)
    return;
  if (A.waiting_for_model)
    finish_reply (TR ("Detenido", "Stopped"));
  else
    g_cancellable_cancel (A.cancel);
}

static void
on_send_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  (void) user_data;

  if (is_streaming ())
    {
      stop_generation ();
      return;
    }

  GtkTextBuffer *buf = gtk_text_view_get_buffer (A.input);
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds (buf, &start, &end);
  g_autofree char *text = gtk_text_buffer_get_text (buf, &start, &end, FALSE);
  if (!A.model)
    {
      gtk_menu_button_popup (A.model_button);
      return;
    }
  if (send_text (text))
    gtk_text_buffer_set_text (buf, "", 0);
}

static gboolean
on_input_key (GtkEventControllerKey *ctrl, guint keyval, guint keycode,
              GdkModifierType state, gpointer user_data)
{
  (void) ctrl;
  (void) keycode;
  (void) user_data;
  if ((keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) &&
      !(state & (GDK_SHIFT_MASK | GDK_CONTROL_MASK)))
    {
      if (!is_streaming ())
        on_send_clicked (A.send, NULL);
      return TRUE;
    }
  return FALSE;
}

static void
on_buffer_changed (GtkTextBuffer *buf, gpointer user_data)
{
  (void) user_data;
  gtk_widget_set_visible (A.placeholder, gtk_text_buffer_get_char_count (buf) == 0);
}

/* ---- models: picker --------------------------------------------------- */

static void
select_model (const char *name)
{
  if (A.model_button)
    gtk_menu_button_popdown (A.model_button);

  if (g_strcmp0 (name, A.model) == 0 && flm_state () != FLM_ERROR && flm_state () != FLM_STOPPED)
    return;

  if (A.reply)
    stop_generation ();

  g_free (A.model);
  A.model = g_strdup (name);
  settings_save ();
  if (memlock_ok ())
    flm_load (A.model, effective_pmode ());
  else
    show_memlock_dialog ();
  update_header (NULL);
  md_refresh ();
}

static void
on_picker_activated (GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
  (void) box;
  (void) user_data;
  select_model (g_object_get_data (G_OBJECT (row), "model"));
}

static char *
model_subtitle (const FlmModel *m)
{
  ModelInfo info;
  catalog_describe (m, &info);
  const char *family = catalog_family (m->name);
  g_autofree char *size = size_text (&info);
  gboolean tools = m->labels && g_strv_contains ((const char *const *) m->labels, "tool-calling");
  return g_strdup_printf ("%s%s%s%s%s", family ? family : "", family ? " · " : "", size,
                          tools ? " · " : "", tools ? TR ("herramientas", "tools") : "");
}

static void
rebuild_picker (void)
{
  gtk_list_box_remove_all (A.picker);

  for (guint i = 0; A.installed && i < A.installed->len; i++)
    {
      FlmModel *m = A.installed->pdata[i];
      ModelInfo info;
      catalog_describe (m, &info);
      if (!info.chat)
        continue;

      GtkWidget *row = adw_action_row_new ();
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), m->name);
      adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
      g_autofree char *sub = model_subtitle (m);
      adw_action_row_set_subtitle (ADW_ACTION_ROW (row), sub);
      gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), TRUE);

      GtkWidget *check = gtk_image_new_from_icon_name ("object-select-symbolic");
      gtk_widget_add_css_class (check, "accent");
      gtk_widget_set_opacity (check, g_strcmp0 (m->name, A.model) == 0 ? 1 : 0);
      adw_action_row_add_suffix (ADW_ACTION_ROW (row), check);

      g_object_set_data_full (G_OBJECT (row), "model", g_strdup (m->name), g_free);
      gtk_list_box_append (A.picker, row);
    }
}

static void
on_installed_listed (GPtrArray *models, const char *error, gpointer user_data)
{
  (void) user_data;
  if (A.quitting)
    return;
  if (!models)
    {
      toast ("%s: %s", TR ("No se pudo listar los modelos", "Could not list models"), error);
      return;
    }

  g_clear_pointer (&A.installed, g_ptr_array_unref);
  A.installed = g_ptr_array_ref (models);

  if (A.model && !installed_find (A.model) && !flm_is_external ())
    {
      flm_stop ();
      g_clear_pointer (&A.model, g_free);
      settings_save ();
    }

  rebuild_picker ();
  if (!A.conv || A.conv->msgs->len == 0)
    show_empty_state ();
  update_header (NULL);
  md_refresh ();
}

static void
refresh_models (void)
{
  A.flm_present = flm_available ();
  if (!A.flm_present)
    {
      g_clear_pointer (&A.installed, g_ptr_array_unref);
      rebuild_picker ();
      if (!A.conv || A.conv->msgs->len == 0)
        show_empty_state ();
      update_header (NULL);
      md_refresh ();
      return;
    }
  flm_list_models ("installed", on_installed_listed, NULL);
}

/* ---- models: dialog --------------------------------------------------- */

typedef struct {
  char      *name;
  GtkWidget *row;
  GtkWidget *button;
  GtkWidget *bar;
} PullCtx;

static void
pull_progress (const char *line, double fraction, gpointer user_data)
{
  PullCtx *ctx = user_data;
  if (fraction >= 0)
    {
      gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (ctx->bar), fraction);
      g_autofree char *pct = g_strdup_printf ("%s %.0f %%", TR ("Descargando…", "Downloading…"), fraction * 100);
      adw_action_row_set_subtitle (ADW_ACTION_ROW (ctx->row), pct);
    }
  else
    {
      gtk_progress_bar_pulse (GTK_PROGRESS_BAR (ctx->bar));
      adw_action_row_set_subtitle (ADW_ACTION_ROW (ctx->row), line);
    }
}

static void
pull_done (gboolean ok, const char *message, gpointer user_data)
{
  PullCtx *ctx = user_data;

  g_hash_table_remove (A.pulling, ctx->name);
  if (!A.quitting)
    {
      gtk_widget_set_visible (ctx->bar, FALSE);
      if (ok)
        {
          toast ("%s %s", ctx->name, TR ("listo para usar", "ready to use"));
          refresh_models ();
        }
      else
        {
          toast ("%s %s", TR ("Falló la descarga de", "Download failed:"), ctx->name);
          adw_action_row_set_subtitle (ADW_ACTION_ROW (ctx->row), message ? message : "Error");
          gtk_widget_set_sensitive (ctx->button, TRUE);
        }
    }

  g_object_unref (ctx->row);
  g_object_unref (ctx->button);
  g_object_unref (ctx->bar);
  g_free (ctx->name);
  g_free (ctx);
}

static void
on_pull_clicked (GtkButton *button, gpointer user_data)
{
  GtkWidget *row = user_data;
  PullCtx *ctx = g_new0 (PullCtx, 1);
  ctx->name = g_strdup (g_object_get_data (G_OBJECT (row), "model"));
  ctx->row = TRACK (g_object_ref (row));
  ctx->button = g_object_ref (GTK_WIDGET (button));
  ctx->bar = g_object_ref (g_object_get_data (G_OBJECT (row), "bar"));

  g_hash_table_add (A.pulling, g_strdup (ctx->name));
  gtk_widget_set_sensitive (GTK_WIDGET (button), FALSE);
  gtk_widget_set_visible (ctx->bar, TRUE);
  adw_action_row_set_subtitle (ADW_ACTION_ROW (row), TR ("Iniciando descarga…", "Starting download…"));
  flm_pull (ctx->name, pull_progress, pull_done, ctx);
}

static GtkWidget *
badge (const char *text, const char *css)
{
  GtkWidget *l = gtk_label_new (text);
  gtk_widget_add_css_class (l, "badge");
  if (css)
    gtk_widget_add_css_class (l, css);
  gtk_widget_set_valign (l, GTK_ALIGN_CENTER);
  return l;
}

static GtkWidget *
available_row (FlmModel *m, const ModelInfo *info)
{
  GtkWidget *row = adw_action_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), m->name);
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);

  g_autoptr (GString) sub = g_string_new (catalog_family (m->name));
  g_autofree char *size = size_text (info);
  if (sub->len)
    g_string_append (sub, " · ");
  g_string_append (sub, size);
  if (info->vision)
    g_string_append_printf (sub, " · %s", TR ("visión", "vision"));
  if (info->reasoning)
    g_string_append_printf (sub, " · %s", TR ("razona", "reasoning"));
  adw_action_row_set_subtitle (ADW_ACTION_ROW (row), sub->str);
  g_object_set_data_full (G_OBJECT (row), "model", g_strdup (m->name), g_free);

  if (catalog_recommended (info))
    adw_action_row_add_suffix (ADW_ACTION_ROW (row), badge (TR ("Recomendado", "Recommended"), "accent"));

  GtkWidget *bar = gtk_progress_bar_new ();
  gtk_widget_set_valign (bar, GTK_ALIGN_CENTER);
  gtk_widget_set_size_request (bar, 80, -1);
  gtk_widget_set_visible (bar, FALSE);
  g_object_set_data (G_OBJECT (row), "bar", bar);

  GtkWidget *btn = icon_button ("folder-download-symbolic", TR ("Descargar", "Download"));
  g_signal_connect (btn, "clicked", G_CALLBACK (on_pull_clicked), row);
  if (g_hash_table_contains (A.pulling, m->name))
    {
      gtk_widget_set_sensitive (btn, FALSE);
      adw_action_row_set_subtitle (ADW_ACTION_ROW (row), TR ("Descargando…", "Downloading…"));
    }

  adw_action_row_add_suffix (ADW_ACTION_ROW (row), bar);
  adw_action_row_add_suffix (ADW_ACTION_ROW (row), btn);
  return row;
}

typedef struct {
  FlmModel *model;
  ModelInfo info;
} Candidate;

static int
by_recommendation (gconstpointer a, gconstpointer b)
{
  const Candidate *ca = a;
  const Candidate *cb = b;
  gboolean ra = catalog_recommended (&ca->info);
  gboolean rb = catalog_recommended (&cb->info);
  if (ra != rb)
    return rb - ra;
  if (ca->info.params_b != cb->info.params_b)
    return ca->info.params_b > cb->info.params_b ? -1 : 1;
  return g_strcmp0 (ca->model->name, cb->model->name);
}

static void
on_available_listed (GPtrArray *models, const char *error, gpointer user_data)
{
  ModelsDialog *md = &A.md;
  if (A.quitting || !md->dialog || GPOINTER_TO_UINT (user_data) != md->gen)
    return;

  gtk_list_box_remove_all (md->available);
  if (!models)
    {
      gtk_label_set_text (md->available_msg, error);
      gtk_stack_set_visible_child_name (md->available_stack, "message");
      return;
    }

  g_autoptr (GArray) cands = g_array_new (FALSE, FALSE, sizeof (Candidate));
  guint hidden = 0;
  for (guint i = 0; i < models->len; i++)
    {
      Candidate c = { .model = models->pdata[i] };
      catalog_describe (c.model, &c.info);
      if (!c.info.chat)
        continue;
      if (catalog_fit (&c.info, md->sys) != FIT_OK)
        {
          hidden++;
          continue;
        }
      g_array_append_val (cands, c);
    }
  g_array_sort (cands, by_recommendation);

  for (guint i = 0; i < cands->len; i++)
    {
      Candidate *c = &g_array_index (cands, Candidate, i);
      gtk_list_box_append (md->available, available_row (c->model, &c->info));
    }

  if (hidden > 0)
    {
      g_autofree char *t = g_strdup_printf (TR ("%u modelos más grandes se ocultaron porque no caben en tu equipo.",
                                                "%u larger models are hidden because they don't fit this computer."),
                                            hidden);
      gtk_label_set_text (md->hidden, t);
    }
  gtk_widget_set_visible (GTK_WIDGET (md->hidden), hidden > 0);

  if (cands->len == 0)
    {
      gtk_label_set_text (md->available_msg, TR ("Ya tienes todos los modelos compatibles.",
                                                 "You already have every compatible model."));
      gtk_stack_set_visible_child_name (md->available_stack, "message");
    }
  else
    gtk_stack_set_visible_child_name (md->available_stack, "list");
}

static void
on_remove_done (gboolean ok, const char *message, gpointer user_data)
{
  g_autofree char *name = user_data;
  if (A.quitting)
    return;
  if (ok)
    toast ("%s %s", name, TR ("eliminado", "removed"));
  else
    toast ("%s %s: %s", TR ("No se pudo eliminar", "Could not remove"), name, message);
  refresh_models ();
}

static void
on_remove_response (AdwAlertDialog *dialog, const char *response, gpointer user_data)
{
  (void) dialog;
  const char *name = user_data;
  if (!g_str_equal (response, "remove"))
    return;

  if (g_strcmp0 (name, A.model) == 0)
    {
      if (A.reply)
        stop_generation ();
      flm_stop ();
      g_clear_pointer (&A.model, g_free);
      settings_save ();
      update_header (NULL);
    }
  flm_remove (name, on_remove_done, g_strdup (name));
}

static void
free_closure_string (gpointer data, GClosure *closure)
{
  (void) closure;
  g_free (data);
}

static void
on_remove_clicked (GtkButton *button, gpointer user_data)
{
  (void) user_data;
  const char *name = g_object_get_data (G_OBJECT (button), "model");
  AdwDialog *d = TRACK (adw_alert_dialog_new (NULL, NULL));
  adw_alert_dialog_format_heading (ADW_ALERT_DIALOG (d), TR ("¿Eliminar %s?", "Remove %s?"), name);
  adw_alert_dialog_set_body (ADW_ALERT_DIALOG (d), TR ("Se borrará del disco. Puedes volver a descargarlo cuando quieras.",
                                                       "It will be deleted from disk. You can download it again anytime."));
  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (d), "cancel", TR ("Cancelar", "Cancel"),
                                  "remove", TR ("Eliminar", "Remove"), NULL);
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (d), "remove", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (d), "cancel");
  g_signal_connect_data (d, "response", G_CALLBACK (on_remove_response),
                         g_strdup (name), free_closure_string, 0);
  adw_dialog_present (d, GTK_WIDGET (A.win));
}

static void
on_use_clicked (GtkButton *button, gpointer user_data)
{
  (void) user_data;
  select_model (g_object_get_data (G_OBJECT (button), "model"));
}

static GtkWidget *
installed_row (FlmModel *m)
{
  ModelInfo info;
  catalog_describe (m, &info);

  GtkWidget *row = adw_action_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), m->name);
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
  g_autofree char *sub = model_subtitle (m);
  adw_action_row_set_subtitle (ADW_ACTION_ROW (row), sub);

  if (g_strcmp0 (m->name, A.model) == 0)
    adw_action_row_add_suffix (ADW_ACTION_ROW (row), badge (TR ("En uso", "In use"), "accent"));
  else if (info.chat)
    {
      GtkWidget *use = gtk_button_new_with_label (TR ("Usar", "Use"));
      gtk_widget_set_valign (use, GTK_ALIGN_CENTER);
      gtk_widget_add_css_class (use, "flat");
      g_object_set_data_full (G_OBJECT (use), "model", g_strdup (m->name), g_free);
      g_signal_connect (use, "clicked", G_CALLBACK (on_use_clicked), NULL);
      adw_action_row_add_suffix (ADW_ACTION_ROW (row), use);
    }

  GtkWidget *del = icon_button ("user-trash-symbolic", TR ("Eliminar del disco", "Delete from disk"));
  g_object_set_data_full (G_OBJECT (del), "model", g_strdup (m->name), g_free);
  g_signal_connect (del, "clicked", G_CALLBACK (on_remove_clicked), NULL);
  adw_action_row_add_suffix (ADW_ACTION_ROW (row), del);
  return row;
}

static void
md_refresh (void)
{
  ModelsDialog *md = &A.md;
  if (!md->dialog)
    return;

  gtk_list_box_remove_all (md->installed);
  for (guint i = 0; A.installed && i < A.installed->len; i++)
    gtk_list_box_append (md->installed, installed_row (A.installed->pdata[i]));

  md->gen++;
  if (!A.flm_present)
    {
      gtk_label_set_text (md->available_msg, TR ("Primero instala FastFlowLM.", "Install FastFlowLM first."));
      gtk_stack_set_visible_child_name (md->available_stack, "message");
    }
  else if (!md->sys->npu_supported)
    {
      gtk_label_set_text (md->available_msg,
                          TR ("Tu NPU no es compatible con FastFlowLM (necesita AMD XDNA 2).",
                              "Your NPU is not supported by FastFlowLM (needs AMD XDNA 2)."));
      gtk_stack_set_visible_child_name (md->available_stack, "message");
    }
  else
    {
      gtk_stack_set_visible_child_name (md->available_stack, "loading");
      flm_list_models ("not-installed", on_available_listed, GUINT_TO_POINTER (md->gen));
    }
}

static GtkWidget *
status_icon (gboolean ok)
{
  GtkWidget *img = gtk_image_new_from_icon_name (ok ? "npu-ok-symbolic" : "dialog-warning-symbolic");
  gtk_widget_add_css_class (img, ok ? "success" : "warning");
  return img;
}

static void
add_info_row (AdwPreferencesGroup *group, const char *title, const char *value, int ok)
{
  GtkWidget *row = adw_action_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
  adw_action_row_set_subtitle (ADW_ACTION_ROW (row), value ? value : "—");
  adw_action_row_set_subtitle_selectable (ADW_ACTION_ROW (row), TRUE);
  gtk_widget_add_css_class (row, "property");
  if (ok >= 0)
    adw_action_row_add_suffix (ADW_ACTION_ROW (row), status_icon (ok));
  adw_preferences_group_add (group, row);
}

static GtkWidget *
build_system_group (const SysInfo *sys)
{
  AdwPreferencesGroup *g = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (g, TR ("Tu equipo", "Your computer"));

  g_autofree char *npu = sys->npu_present
                           ? g_strdup_printf ("%s%s%s", sys->npu_name,
                                              sys->npu_firmware ? " · firmware " : "",
                                              sys->npu_firmware ? sys->npu_firmware : "")
                           : g_strdup (TR ("No se detectó una NPU AMD", "No AMD NPU detected"));
  add_info_row (g, "NPU", npu, sys->npu_supported);

  g_autofree char *ram = g_strdup_printf (TR ("%.1f GB · caben modelos que usen hasta %.1f GB",
                                              "%.1f GB · fits models using up to %.1f GB"),
                                          sys->ram_total_gb, catalog_max_footprint (sys));
  add_info_row (g, TR ("Memoria", "Memory"), ram, -1);

  g_autofree char *disk = g_strdup_printf (TR ("%.0f GB libres", "%.0f GB free"), sys->disk_free_gb);
  add_info_row (g, TR ("Disco", "Disk"), disk, sys->disk_free_gb > 6);

  add_info_row (g, TR ("Procesador", "Processor"), sys->cpu, -1);

  gboolean flm = flm_available ();
  add_info_row (g, "FastFlowLM", flm ? TR ("Instalado", "Installed") : TR ("No instalado", "Not installed"), flm);
  if (flm && !memlock_ok ())
    add_info_row (g, TR ("Memoria de la NPU", "NPU memory"),
                  TR ("Falta un ajuste del sistema (ver aviso en el chat)", "System setting needed (see the chat banner)"), FALSE);

  return GTK_WIDGET (g);
}

static void
on_models_dialog_closed (AdwDialog *dialog, gpointer user_data)
{
  (void) dialog;
  (void) user_data;
  g_clear_pointer (&A.md.sys, sysinfo_free);
  A.md.dialog = NULL;
  A.md.installed = NULL;
  A.md.available = NULL;
  A.md.gen++;
}

static GtkWidget *
boxed_list (const char *placeholder)
{
  GtkWidget *list = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (list), GTK_SELECTION_NONE);
  gtk_widget_add_css_class (list, "boxed-list");
  if (placeholder)
    {
      GtkWidget *ph = dim_label (placeholder, "placeholder-row");
      gtk_label_set_xalign (GTK_LABEL (ph), 0.5);
      gtk_list_box_set_placeholder (GTK_LIST_BOX (list), ph);
    }
  return list;
}

static void
open_models_dialog (gboolean download)
{
  ModelsDialog *md = &A.md;
  if (md->dialog)
    {
      adw_view_stack_set_visible_child_name (md->stack, download ? "download" : "installed");
      return;
    }

  md->sys = sysinfo_get ();
  md->dialog = TRACK (adw_dialog_new ());
  adw_dialog_set_title (md->dialog, TR ("Modelos", "Models"));
  adw_dialog_set_content_width (md->dialog, 580);
  adw_dialog_set_content_height (md->dialog, 680);

  GtkWidget *tv = adw_toolbar_view_new ();
  GtkWidget *header = adw_header_bar_new ();
  md->stack = ADW_VIEW_STACK (adw_view_stack_new ());
  GtkWidget *switcher = adw_view_switcher_new ();
  adw_view_switcher_set_stack (ADW_VIEW_SWITCHER (switcher), md->stack);
  adw_view_switcher_set_policy (ADW_VIEW_SWITCHER (switcher), ADW_VIEW_SWITCHER_POLICY_WIDE);
  adw_header_bar_set_title_widget (ADW_HEADER_BAR (header), switcher);
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (tv), header);

  /* Installed */
  GtkWidget *ipage = adw_preferences_page_new ();
  AdwPreferencesGroup *ig = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_description (ig, TR ("Modelos guardados en tu equipo.", "Models stored on this computer."));
  md->installed = GTK_LIST_BOX (boxed_list (TR ("Todavía no tienes modelos", "No models yet")));
  adw_preferences_group_add (ig, GTK_WIDGET (md->installed));
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (ipage), ig);
  adw_view_stack_add_titled_with_icon (md->stack, ipage, "installed", TR ("Instalados", "Installed"),
                                       "drive-harddisk-symbolic");

  /* Download */
  GtkWidget *dpage = adw_preferences_page_new ();
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (dpage), ADW_PREFERENCES_GROUP (build_system_group (md->sys)));

  AdwPreferencesGroup *cg = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (cg, TR ("Compatibles con tu equipo", "Compatible with your computer"));
  adw_preferences_group_set_description (cg, TR ("Solo se muestran los modelos que caben en tu NPU, memoria y disco.",
                                                 "Only models that fit your NPU, memory and disk are shown."));

  md->available = GTK_LIST_BOX (boxed_list (NULL));
  GtkWidget *spinner = gtk_spinner_new ();
  gtk_spinner_set_spinning (GTK_SPINNER (spinner), TRUE);
  gtk_widget_set_size_request (spinner, 32, 32);
  gtk_widget_set_margin_top (spinner, 24);
  gtk_widget_set_margin_bottom (spinner, 24);
  md->available_msg = GTK_LABEL (dim_label (NULL, NULL));
  gtk_label_set_xalign (md->available_msg, 0.5);
  gtk_label_set_justify (md->available_msg, GTK_JUSTIFY_CENTER);
  gtk_widget_set_margin_top (GTK_WIDGET (md->available_msg), 18);

  md->available_stack = GTK_STACK (gtk_stack_new ());
  gtk_stack_set_vhomogeneous (md->available_stack, FALSE);
  gtk_stack_add_named (md->available_stack, spinner, "loading");
  gtk_stack_add_named (md->available_stack, GTK_WIDGET (md->available), "list");
  gtk_stack_add_named (md->available_stack, GTK_WIDGET (md->available_msg), "message");
  adw_preferences_group_add (cg, GTK_WIDGET (md->available_stack));

  md->hidden = GTK_LABEL (dim_label (NULL, "caption"));
  gtk_widget_set_margin_top (GTK_WIDGET (md->hidden), 12);
  gtk_widget_set_visible (GTK_WIDGET (md->hidden), FALSE);
  adw_preferences_group_add (cg, GTK_WIDGET (md->hidden));
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (dpage), cg);

  adw_view_stack_add_titled_with_icon (md->stack, dpage, "download", TR ("Descargar", "Download"),
                                       "folder-download-symbolic");
  adw_view_stack_set_visible_child_name (md->stack, download ? "download" : "installed");

  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (tv), GTK_WIDGET (md->stack));
  adw_dialog_set_child (md->dialog, tv);
  g_signal_connect (md->dialog, "closed", G_CALLBACK (on_models_dialog_closed), NULL);
  adw_dialog_present (md->dialog, GTK_WIDGET (A.win));
  md_refresh ();
}

/* ---- server state ----------------------------------------------------- */

static void
on_flm_state (FlmState state, const char *message, gpointer user_data)
{
  (void) user_data;
  if (A.quitting)
    return;

  if (A.reply && A.waiting_for_model)
    {
      if (state == FLM_READY)
        {
          A.waiting_for_model = FALSE;
          begin_reply ();
        }
      else if (state == FLM_ERROR || state == FLM_STOPPED)
        finish_reply (message ? message : TR ("Detenido", "Stopped"));
    }

  if (state == FLM_ERROR && message && g_regex_match_simple ("memlock|mmap", message, G_REGEX_CASELESS, 0))
    {
      show_memlock_dialog ();
      message = TR ("La NPU no pudo reservar memoria", "The NPU could not reserve memory");
    }
  else if (state == FLM_ERROR && message)
    toast ("%s", message);
  if (state == FLM_READY)
    arm_idle_timer ();
  else
    disarm_idle ();
  update_header (message);
}

static void
on_detected (gboolean external, gpointer user_data)
{
  (void) user_data;
  if (A.quitting)
    return;
  /* Load the last model right away only when plugged in; on battery it
   * loads on the first message instead. */
  if (!external && A.model && A.flm_present && !power_on_battery () && memlock_ok ())
    flm_load (A.model, effective_pmode ());
  update_header (NULL);
}

/* ---- updates ----------------------------------------------------------- */

static void
on_update_dialog_response (AdwAlertDialog *dialog, const char *response, gpointer user_data)
{
  (void) dialog;
  (void) user_data;
  if (g_str_equal (response, "restart"))
    {
      A.restart_after_exit = TRUE;
      gtk_window_close (A.win);
    }
}

static void
on_update_dialog_closed (AdwDialog *dialog, gpointer user_data)
{
  (void) dialog;
  (void) user_data;
  A.update_dialog = NULL;
  g_clear_object (&A.update_bar);
}

static void
update_progress (double fraction, gpointer user_data)
{
  (void) user_data;
  if (A.update_bar)
    gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (A.update_bar), fraction);
}

static void
update_installed (gboolean ok, const char *message, gpointer user_data)
{
  (void) user_data;
  if (A.quitting || !A.update_dialog)
    return;
  AdwAlertDialog *d = ADW_ALERT_DIALOG (A.update_dialog);
  gtk_widget_set_visible (A.update_bar, FALSE);
  adw_dialog_set_can_close (A.update_dialog, TRUE);
  if (ok)
    {
      adw_alert_dialog_set_heading (d, TR ("Actualización lista", "Update ready"));
      adw_alert_dialog_set_body (d, TR ("Reinicia NPU Chat para usar la versión nueva.",
                                        "Restart NPU Chat to use the new version."));
      adw_alert_dialog_add_responses (d, "later", TR ("Más tarde", "Later"),
                                      "restart", TR ("Reiniciar ahora", "Restart now"), NULL);
      adw_alert_dialog_set_response_appearance (d, "restart", ADW_RESPONSE_SUGGESTED);
      adw_alert_dialog_set_default_response (d, "restart");
    }
  else
    {
      adw_alert_dialog_set_heading (d, TR ("No se pudo actualizar", "Update failed"));
      adw_alert_dialog_set_body (d, message);
      adw_alert_dialog_add_response (d, "close", TR ("Cerrar", "Close"));
    }
}

static void
act_install_update (GSimpleAction *a, GVariant *p, gpointer d)
{
  (void) a; (void) p; (void) d;
  if (!A.update || A.update_dialog)
    return;

  AdwDialog *dialog = TRACK (adw_alert_dialog_new (NULL, NULL));
  adw_alert_dialog_format_heading (ADW_ALERT_DIALOG (dialog), TR ("Descargando NPU Chat %s…", "Downloading NPU Chat %s…"),
                                   A.update->version);
  adw_alert_dialog_set_body (ADW_ALERT_DIALOG (dialog), TR ("Se verifica la descarga antes de instalarla.",
                                                            "The download is verified before it is installed."));
  A.update_bar = g_object_ref (gtk_progress_bar_new ());
  adw_alert_dialog_set_extra_child (ADW_ALERT_DIALOG (dialog), A.update_bar);
  adw_dialog_set_can_close (dialog, FALSE);
  g_signal_connect (dialog, "response", G_CALLBACK (on_update_dialog_response), NULL);
  g_signal_connect (dialog, "closed", G_CALLBACK (on_update_dialog_closed), NULL);
  A.update_dialog = dialog;
  adw_dialog_present (dialog, GTK_WIDGET (A.win));
  updater_install (A.update, update_progress, update_installed, NULL);
}

static void
act_open_release (GSimpleAction *a, GVariant *p, gpointer d)
{
  (void) a; (void) p; (void) d;
  if (!A.update)
    return;
  g_autoptr (GtkUriLauncher) launcher = gtk_uri_launcher_new (A.update->page_url);
  gtk_uri_launcher_launch (launcher, A.win, NULL, NULL, NULL);
}

static void
on_update_checked (UpdateInfo *info, const char *error, gpointer user_data)
{
  gboolean manual = GPOINTER_TO_INT (user_data);
  if (A.quitting)
    {
      update_info_free (info);
      return;
    }
  if (!error)
    {
      A.last_update_check = g_get_real_time () / G_USEC_PER_SEC;
      settings_save ();
    }
  if (!info)
    {
      if (manual)
        toast ("%s", error ? TR ("No se pudo buscar actualizaciones", "Could not check for updates")
                           : TR ("Tienes la versión más reciente", "You have the latest version"));
      return;
    }

  update_info_free (A.update);
  A.update = info;
  g_autofree char *title = g_strdup_printf (TR ("NPU Chat %s está disponible", "NPU Chat %s is available"), info->version);
  AdwToast *t = adw_toast_new (title);
  adw_toast_set_timeout (t, 0);
  if (updater_can_self_update () && info->appimage_url && info->sha256_url)
    {
      adw_toast_set_button_label (t, TR ("Actualizar", "Update"));
      adw_toast_set_action_name (t, "win.install-update");
    }
  else
    {
      adw_toast_set_button_label (t, TR ("Ver", "View"));
      adw_toast_set_action_name (t, "win.open-release");
    }
  adw_toast_overlay_add_toast (A.toasts, t);
}

static gboolean
auto_update_check (gpointer user_data)
{
  (void) user_data;
  gint64 now = g_get_real_time () / G_USEC_PER_SEC;
  if (A.auto_update && !A.quitting && now - A.last_update_check > 24 * 3600)
    updater_check (on_update_checked, GINT_TO_POINTER (FALSE));
  return G_SOURCE_REMOVE;
}

static void
on_check_updates_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  (void) user_data;
  updater_check (on_update_checked, GINT_TO_POINTER (TRUE));
}

static void
on_auto_update_toggled (AdwSwitchRow *row, GParamSpec *pspec, gpointer user_data)
{
  (void) pspec;
  (void) user_data;
  A.auto_update = adw_switch_row_get_active (row);
  settings_save ();
}

/* ---- preferences ------------------------------------------------------ */

static const char *theme_values[] = { "system", "light", "dark" };
static const char *lang_values[] = { "es", "en", "auto" };
static const char *pmode_values[] = { "auto", "powersaver", "balanced", "performance", "turbo", "" };

static guint
index_of (const char *const *values, guint n, const char *v)
{
  for (guint i = 0; i < n; i++)
    if (g_strcmp0 (values[i], v) == 0)
      return i;
  return 0;
}

static void
on_theme_selected (AdwComboRow *row, GParamSpec *pspec, gpointer user_data)
{
  (void) pspec;
  (void) user_data;
  g_free (A.theme);
  A.theme = g_strdup (theme_values[adw_combo_row_get_selected (row)]);
  apply_theme ();
  settings_save ();
}

static void
on_lang_selected (AdwComboRow *row, GParamSpec *pspec, gpointer user_data)
{
  (void) pspec;
  (void) user_data;
  g_free (A.lang);
  A.lang = g_strdup (lang_values[adw_combo_row_get_selected (row)]);
  A.lang_changed = TRUE;
  settings_save ();
}

static void
on_pmode_selected (AdwComboRow *row, GParamSpec *pspec, gpointer user_data)
{
  (void) pspec;
  (void) user_data;
  g_free (A.pmode);
  A.pmode = g_strdup (pmode_values[adw_combo_row_get_selected (row)]);
  settings_save ();
  update_power_bar ();
  if (A.reply)
    A.power_reload_pending = TRUE;
  else if (A.model && !flm_is_external () &&
           (flm_state () == FLM_READY || flm_state () == FLM_LOADING) &&
           g_strcmp0 (flm_loaded_pmode (), effective_pmode ()) != 0)
    flm_load (A.model, effective_pmode ());
}

static void
on_idle_toggled (AdwSwitchRow *row, GParamSpec *pspec, gpointer user_data)
{
  (void) pspec;
  (void) user_data;
  A.idle_unload = adw_switch_row_get_active (row);
  settings_save ();
  arm_idle_timer ();
}

static void
on_skill_toggled (AdwSwitchRow *row, GParamSpec *pspec, gpointer user_data)
{
  (void) pspec;
  (void) user_data;
  const char *id = g_object_get_data (G_OBJECT (row), "id");
  if (adw_switch_row_get_active (row))
    g_hash_table_add (A.skills_on, g_strdup (id));
  else
    g_hash_table_remove (A.skills_on, id);
  settings_save ();
}

static void
on_concise_toggled (AdwSwitchRow *row, GParamSpec *pspec, gpointer user_data)
{
  (void) pspec;
  (void) user_data;
  A.concise = adw_switch_row_get_active (row);
  settings_save ();
}

static gboolean
rebuild_idle (gpointer user_data)
{
  (void) user_data;
  if (A.md.dialog)
    adw_dialog_force_close (A.md.dialog);
  i18n_set (A.lang);
  build_ui ();
  refresh_chat_list ();
  render_conversation ();
  rebuild_picker ();
  update_header (NULL);
  return G_SOURCE_REMOVE;
}

static void
on_copilot_key_done (gboolean ok, gpointer user_data)
{
  GtkWidget *btn = user_data; /* kept alive by the reference taken when it was clicked */
  gboolean now = copilotkey_installed ();
  gtk_button_set_label (GTK_BUTTON (btn), now ? TR ("Quitar", "Remove") : TR ("Activar", "Enable"));
  gtk_widget_set_sensitive (btn, TRUE);
  g_object_unref (btn);
  toast ("%s", ok ? (now ? TR ("Tecla Copilot lista: abre NPU Chat", "Copilot key ready: it opens NPU Chat")
                         : TR ("Tecla Copilot restaurada", "Copilot key restored"))
                  : TR ("No se pudo cambiar la tecla Copilot", "Could not change the Copilot key"));
}

/* The Copilot key reports Shift+Super+F23 (hardware keycode 201), shown by layouts
 * as "Assistant". Waiting for that exact press is how the app knows the laptop has one. */
static gboolean
copilot_key_pressed (GtkEventControllerKey *ctl, guint keyval, guint keycode, GdkModifierType state, gpointer user_data)
{
  (void) ctl;
  if (keyval != 0x10081247 && keycode != 201)
    return FALSE;
  if (!(state & GDK_SHIFT_MASK) || !(state & GDK_SUPER_MASK))
    return FALSE;
  AdwDialog *dialog = ADW_DIALOG (user_data);
  GtkWidget *btn = g_object_get_data (G_OBJECT (dialog), "button");
  adw_dialog_force_close (dialog);
  copilotkey_set (TRUE, on_copilot_key_done, g_object_ref (btn));
  return TRUE;
}

static void
on_copilot_learn_response (AdwAlertDialog *dialog, const char *response, gpointer user_data)
{
  (void) response;
  (void) dialog;
  gtk_widget_set_sensitive (GTK_WIDGET (user_data), TRUE);
}

static void
on_copilot_key_clicked (GtkButton *btn, gpointer user_data)
{
  (void) user_data;
  if (copilotkey_installed ())
    {
      gtk_widget_set_sensitive (GTK_WIDGET (btn), FALSE);
      copilotkey_set (FALSE, on_copilot_key_done, g_object_ref (btn));
      return;
    }
  AdwDialog *dialog = adw_alert_dialog_new (TR ("Pulsa la tecla Copilot", "Press the Copilot key"),
                                            TR ("Así NPU Chat comprueba que tu laptop la tiene. Si no pasa nada, no tiene una.",
                                                "This is how NPU Chat checks that your laptop has one. If nothing happens, it does not."));
  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "cancel", TR ("Cancelar", "Cancel"));
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dialog), "cancel");
  GtkEventController *keys = gtk_event_controller_key_new ();
  gtk_event_controller_set_propagation_phase (keys, GTK_PHASE_CAPTURE);
  g_signal_connect (keys, "key-pressed", G_CALLBACK (copilot_key_pressed), dialog);
  gtk_widget_add_controller (GTK_WIDGET (dialog), keys);
  g_object_set_data (G_OBJECT (dialog), "button", btn);
  g_signal_connect (dialog, "response", G_CALLBACK (on_copilot_learn_response), btn);
  adw_dialog_present (dialog, GTK_WIDGET (A.win));
}

static void
on_prefs_closed (AdwDialog *dialog, gpointer user_data)
{
  (void) dialog;
  (void) user_data;
  A.prefs = NULL;
  if (!A.lang_changed)
    return;
  A.lang_changed = FALSE;
  /* Rebuild after the dialog is fully gone. */
  g_idle_add (rebuild_idle, NULL);
}

static GtkWidget *
combo_row (const char *title, const char *subtitle, const char *const *labels, guint selected,
           GCallback cb)
{
  GtkWidget *row = adw_combo_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
  if (subtitle)
    adw_action_row_set_subtitle (ADW_ACTION_ROW (row), subtitle);
  g_autoptr (GtkStringList) model = gtk_string_list_new (labels);
  adw_combo_row_set_model (ADW_COMBO_ROW (row), G_LIST_MODEL (model));
  adw_combo_row_set_selected (ADW_COMBO_ROW (row), selected);
  g_signal_connect (row, "notify::selected", cb, NULL);
  return row;
}

static void
open_preferences (void)
{
  AdwDialog *dialog = TRACK (adw_preferences_dialog_new ());
  adw_dialog_set_title (dialog, TR ("Preferencias", "Preferences"));
  GtkWidget *page = adw_preferences_page_new ();

  AdwPreferencesGroup *look = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (look, TR ("Apariencia", "Appearance"));
  const char *themes[] = { TR ("Sistema", "System"), TR ("Claro", "Light"), TR ("Oscuro", "Dark"), NULL };
  adw_preferences_group_add (look, combo_row (TR ("Tema", "Theme"), NULL, themes,
                                              index_of (theme_values, G_N_ELEMENTS (theme_values), A.theme),
                                              G_CALLBACK (on_theme_selected)));
  const char *langs[] = { "Español", "English", TR ("Automático", "Automatic"), NULL };
  adw_preferences_group_add (look, combo_row (TR ("Idioma", "Language"), NULL, langs,
                                              index_of (lang_values, G_N_ELEMENTS (lang_values), A.lang),
                                              G_CALLBACK (on_lang_selected)));

  AdwPreferencesGroup *npu = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (npu, TR ("Energía", "Power"));
  const char *modes[] = { TR ("Automático", "Automatic"), TR ("Ahorro", "Power saver"),
                          TR ("Equilibrado", "Balanced"), TR ("Rendimiento", "Performance"),
                          "Turbo", TR ("Por defecto de FLM", "FLM default"), NULL };
  adw_preferences_group_add (npu, combo_row (TR ("Modo de la NPU", "NPU mode"),
                                             TR ("Automático: ahorro en batería, rendimiento conectado",
                                                 "Automatic: power saver on battery, performance when plugged in"),
                                             modes, index_of (pmode_values, G_N_ELEMENTS (pmode_values), A.pmode),
                                             G_CALLBACK (on_pmode_selected)));
  GtkWidget *idle = adw_switch_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (idle), TR ("Liberar el modelo en batería", "Unload model on battery"));
  adw_action_row_set_subtitle (ADW_ACTION_ROW (idle), TR ("Tras 10 minutos sin uso; se vuelve a cargar al escribir",
                                                          "After 10 idle minutes; reloads when you type"));
  adw_switch_row_set_active (ADW_SWITCH_ROW (idle), A.idle_unload);
  g_signal_connect (idle, "notify::active", G_CALLBACK (on_idle_toggled), NULL);
  adw_preferences_group_add (npu, idle);

  AdwPreferencesGroup *chat = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (chat, TR ("Respuestas", "Replies"));
  GtkWidget *concise = adw_switch_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (concise), TR ("Respuestas breves", "Short replies"));
  adw_action_row_set_subtitle (ADW_ACTION_ROW (concise),
                               TR ("Mucho más rápidas; pide más detalle cuando lo necesites",
                                   "Much faster; ask for more detail when you need it"));
  adw_switch_row_set_active (ADW_SWITCH_ROW (concise), A.concise);
  g_signal_connect (concise, "notify::active", G_CALLBACK (on_concise_toggled), NULL);
  adw_preferences_group_add (chat, concise);

  AdwPreferencesGroup *keys_group = NULL;
  if (copilotkey_supported ())
    {
      keys_group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
      adw_preferences_group_set_title (keys_group, TR ("Teclado", "Keyboard"));
      GtkWidget *krow = adw_action_row_new ();
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (krow), TR ("Tecla Copilot abre NPU Chat", "Copilot key opens NPU Chat"));
      adw_action_row_set_subtitle (ADW_ACTION_ROW (krow),
                                   TR ("Pide la contraseña una vez. Solo para laptops con tecla Copilot.",
                                       "Asks for your password once. Only for laptops with a Copilot key."));
      GtkWidget *kbtn = gtk_button_new_with_label (copilotkey_installed () ? TR ("Quitar", "Remove") : TR ("Activar", "Enable"));
      gtk_widget_set_valign (kbtn, GTK_ALIGN_CENTER);
      g_signal_connect (kbtn, "clicked", G_CALLBACK (on_copilot_key_clicked), NULL);
      adw_action_row_add_suffix (ADW_ACTION_ROW (krow), kbtn);
      adw_preferences_group_add (keys_group, krow);
    }

  AdwPreferencesGroup *upd = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (upd, TR ("Actualizaciones", "Updates"));
  adw_preferences_group_set_description (upd, TR ("Es la única conexión a internet de NPU Chat: consulta GitHub una vez al día.",
                                                  "NPU Chat's only internet connection: it asks GitHub once a day."));
  GtkWidget *auto_row = adw_switch_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (auto_row), TR ("Buscar actualizaciones automáticamente",
                                                                     "Check for updates automatically"));
  adw_switch_row_set_active (ADW_SWITCH_ROW (auto_row), A.auto_update);
  g_signal_connect (auto_row, "notify::active", G_CALLBACK (on_auto_update_toggled), NULL);
  adw_preferences_group_add (upd, auto_row);
  GtkWidget *now_row = adw_action_row_new ();
  g_autofree char *version_text = g_strdup_printf (TR ("Versión instalada: %s", "Installed version: %s"), NPU_CHAT_VERSION);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (now_row), version_text);
  GtkWidget *now_btn = gtk_button_new_with_label (TR ("Buscar ahora", "Check now"));
  gtk_widget_set_valign (now_btn, GTK_ALIGN_CENTER);
  g_signal_connect (now_btn, "clicked", G_CALLBACK (on_check_updates_clicked), NULL);
  adw_action_row_add_suffix (ADW_ACTION_ROW (now_row), now_btn);
  adw_preferences_group_add (upd, now_row);

  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), look);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), chat);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), npu);
  if (keys_group)
    adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), keys_group);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), upd);
  adw_preferences_dialog_add (ADW_PREFERENCES_DIALOG (dialog), ADW_PREFERENCES_PAGE (page));
  g_signal_connect (dialog, "closed", G_CALLBACK (on_prefs_closed), NULL);
  A.prefs = dialog;
  adw_dialog_present (dialog, GTK_WIDGET (A.win));
}

static void
on_skills_dialog_closed (AdwDialog *dialog, gpointer user_data)
{
  (void) dialog;
  (void) user_data;
  A.skills_dialog = NULL;
}

static void
open_skills_dialog (void)
{
  if (A.skills_dialog)
    return;
  A.skills_dialog = TRACK (adw_dialog_new ());
  adw_dialog_set_title (A.skills_dialog, TR ("Habilidades", "Skills"));
  adw_dialog_set_content_width (A.skills_dialog, 560);
  adw_dialog_set_content_height (A.skills_dialog, 560);
  GtkWidget *page = adw_preferences_page_new ();
  AdwPreferencesGroup *skills_group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
    g_autofree char *skills_hint = g_strdup_printf ("%s%s",
    TR ("El modelo decide cuándo usarlas y te muestra lo que hizo. Funcionan con modelos que admiten herramientas (como qwen3 y qwen3.5).",
        "The model decides when to use them and shows you what it did. They work with models that support tools (such as qwen3 and qwen3.5)."),
    model_supports_tools () || !A.model ? "" : TR (" El modelo actual no las admite.", " The current model does not support them."));
  adw_preferences_group_set_description (skills_group, skills_hint);
  guint n_skills;
  const Skill *all_skills = skills_list (&n_skills);
  for (guint i = 0; i < n_skills; i++)
    {
      GtkWidget *row = adw_switch_row_new ();
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), TR (all_skills[i].name_es, all_skills[i].name_en));
      adw_action_row_set_subtitle (ADW_ACTION_ROW (row), TR (all_skills[i].desc_es, all_skills[i].desc_en));
      adw_switch_row_set_active (ADW_SWITCH_ROW (row), g_hash_table_contains (A.skills_on, all_skills[i].id));
      g_object_set_data (G_OBJECT (row), "id", (gpointer) all_skills[i].id);
      g_signal_connect (row, "notify::active", G_CALLBACK (on_skill_toggled), NULL);
      adw_preferences_group_add (skills_group, row);
    }

  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), skills_group);
  GtkWidget *tv = adw_toolbar_view_new ();
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (tv), adw_header_bar_new ());
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (tv), page);
  adw_dialog_set_child (A.skills_dialog, tv);
  g_signal_connect (A.skills_dialog, "closed", G_CALLBACK (on_skills_dialog_closed), NULL);
  adw_dialog_present (A.skills_dialog, GTK_WIDGET (A.win));
}

/* ---- actions ---------------------------------------------------------- */

#define UNUSED_ACTION_ARGS (void) a; (void) p; (void) d

static void act_new_chat (GSimpleAction *a, GVariant *p, gpointer d) { UNUSED_ACTION_ARGS; new_chat (); }
static void act_models (GSimpleAction *a, GVariant *p, gpointer d) { UNUSED_ACTION_ARGS; open_models_dialog (FALSE); }
static void act_download (GSimpleAction *a, GVariant *p, gpointer d) { UNUSED_ACTION_ARGS; open_models_dialog (TRUE); }
static void act_skills (GSimpleAction *a, GVariant *p, gpointer d) { UNUSED_ACTION_ARGS; open_skills_dialog (); }
static void act_calendar (GSimpleAction *a, GVariant *p, gpointer d) { UNUSED_ACTION_ARGS; calendar_ui_open (GTK_WIDGET (A.win)); }
static void act_prefs (GSimpleAction *a, GVariant *p, gpointer d) { UNUSED_ACTION_ARGS; open_preferences (); }
static void act_refresh (GSimpleAction *a, GVariant *p, gpointer d) { UNUSED_ACTION_ARGS; refresh_models (); }
static void act_retry (GSimpleAction *a, GVariant *p, gpointer d) { UNUSED_ACTION_ARGS; ensure_loaded (); }
static void act_quit (GSimpleAction *a, GVariant *p, gpointer d) { UNUSED_ACTION_ARGS; gtk_window_close (A.win); }

static void
act_about (GSimpleAction *a, GVariant *p, gpointer d)
{
  UNUSED_ACTION_ARGS;
  AdwDialog *about = TRACK (adw_about_dialog_new ());
  adw_about_dialog_set_application_name (ADW_ABOUT_DIALOG (about), "NPU Chat");
  adw_about_dialog_set_application_icon (ADW_ABOUT_DIALOG (about), APP_ID);
  adw_about_dialog_set_version (ADW_ABOUT_DIALOG (about), NPU_CHAT_VERSION);
  adw_about_dialog_set_comments (ADW_ABOUT_DIALOG (about),
                                 TR ("Chat simple con modelos de IA que corren en la NPU de AMD Ryzen AI, usando FastFlowLM.",
                                     "Simple chat with AI models running on the AMD Ryzen AI NPU, powered by FastFlowLM."));
  adw_about_dialog_set_developer_name (ADW_ABOUT_DIALOG (about), "vezzulab");
  adw_about_dialog_set_website (ADW_ABOUT_DIALOG (about), "https://github.com/vezzulab/npuchat");
  adw_about_dialog_set_license_type (ADW_ABOUT_DIALOG (about), GTK_LICENSE_MIT_X11);
  adw_dialog_present (about, GTK_WIDGET (A.win));
}

static void
on_suggestion (GtkButton *button, gpointer user_data)
{
  (void) user_data;
  if (!A.model)
    {
      gtk_menu_button_popup (A.model_button);
      return;
    }
  send_text (gtk_button_get_label (button));
}

static void
on_copy_commands (GtkButton *button, gpointer user_data)
{
  (void) user_data;
  copy_text (GTK_WIDGET (button), g_object_get_data (G_OBJECT (button), "text"));
}

/* ---- assistants ------------------------------------------------------- */

typedef struct {
  AdwDialog   *dialog;
  AdwEntryRow *name;
  AdwEntryRow *emoji;
  GtkTextView *instructions;
  GtkListBox  *libs;
  char        *id; /* NULL when creating */
} Editor;

static Editor *editor_current;

/* The open chat's assistant once it has messages; otherwise the one picked
 * for the next chat. NULL id means the general assistant. */
static const char *
current_assistant_id (void)
{
  if (A.conv && A.conv_saved)
    return A.conv->assistant_id;
  return A.pick_id;
}

static void
update_welcome_header (void)
{
  Assistant *as = assistants_find (A.assistants, current_assistant_id ());
  if (A.team_mode && !(A.conv && A.conv_saved) && A.pick_team->len >= 2)
    {
      g_autoptr (GString) t = g_string_new (NULL);
      for (guint i = 0; i < A.pick_team->len; i++)
        {
          Assistant *m = assistants_find (A.assistants, A.pick_team->pdata[i]);
          if (m)
            g_string_append_printf (t, "%s ", m->emoji);
        }
      g_string_append (t, TR ("Equipo", "Team"));
      adw_status_page_set_title (A.welcome, t->str);
      adw_status_page_set_description (A.welcome, TR ("Pregunta lo que quieras: responde el especialista que corresponda. "
                                                      "Nómbralo para hablarle directo.",
                                                      "Ask anything: the right specialist answers. "
                                                      "Name one to talk to them directly."));
      gtk_widget_set_visible (A.suggestions, FALSE);
      return;
    }
  if (as)
    {
      g_autofree char *title = g_strdup_printf ("%s %s", as->emoji, as->name);
      adw_status_page_set_title (A.welcome, title);
      adw_status_page_set_description (A.welcome, TR ("Escribe tu mensaje para empezar.",
                                                      "Type your message to start."));
    }
  else
    {
      g_autoptr (GDateTime) now = g_date_time_new_now_local ();
      int h = g_date_time_get_hour (now);
      adw_status_page_set_title (A.welcome, h < 12 ? TR ("Buenos días", "Good morning")
                                            : h < 19 ? TR ("Buenas tardes", "Good afternoon")
                                                     : TR ("Buenas noches", "Good evening"));
      adw_status_page_set_description (A.welcome, TR ("¿En qué te ayudo? Todo corre en tu NPU, sin internet.",
                                                      "How can I help? Everything runs on your NPU, offline."));
    }
  gtk_widget_set_visible (A.suggestions, as == NULL);
}

/* Applies an assistant to the open chat (from the next reply on) and to new chats. */
static void
choose_assistant (const char *id)
{
  Assistant *as = assistants_find (A.assistants, id);

  g_free (A.pick_id);
  A.pick_id = g_strdup (as ? as->id : NULL);

  if (A.conv && A.conv_saved)
    {
      g_free (A.conv->assistant_id);
      g_free (A.conv->assistant_name);
      g_free (A.conv->assistant_emoji);
      g_free (A.conv->system);
      A.conv->assistant_id = as ? g_strdup (as->id) : NULL;
      A.conv->assistant_name = as ? g_strdup (as->name) : NULL;
      A.conv->assistant_emoji = as ? g_strdup (as->emoji) : NULL;
      A.conv->system = as ? g_strdup (as->instructions) : NULL;
      g_ptr_array_set_size (A.conv->team, 0);
      store_save (A.conv);
      refresh_chat_list ();
    }
  update_assistant_ui ();
}

/* Whether an assistant is in the team being edited: the open chat's team,
 * or the one picked for the next chat. */
static gboolean
in_current_team (const char *id)
{
  if (A.conv && A.conv_saved)
    {
      for (guint i = 0; i < A.conv->team->len; i++)
        if (g_strcmp0 (((TeamMember *) A.conv->team->pdata[i])->id, id) == 0)
          return TRUE;
      return A.conv->team->len == 0 && g_strcmp0 (A.conv->assistant_id, id) == 0;
    }
  for (guint i = 0; i < A.pick_team->len; i++)
    if (g_str_equal (A.pick_team->pdata[i], id))
      return TRUE;
  return FALSE;
}

/* Adds or removes a member. In an open chat, a one-member team becomes
 * that single assistant and an empty one falls back to General. */
static void
toggle_team_member (const char *id)
{
  Assistant *as = assistants_find (A.assistants, id);
  if (!as)
    return;

  if (!(A.conv && A.conv_saved))
    {
      for (guint i = 0; i < A.pick_team->len; i++)
        if (g_str_equal (A.pick_team->pdata[i], id))
          {
            g_ptr_array_remove_index (A.pick_team, i);
            update_assistant_ui ();
            return;
          }
      g_ptr_array_add (A.pick_team, g_strdup (id));
      update_assistant_ui ();
      return;
    }

  Conversation *c = A.conv;
  if (c->team->len == 0 && c->assistant_id)
    {
      Assistant *cur = assistants_find (A.assistants, c->assistant_id);
      g_ptr_array_add (c->team, team_member_new (c->assistant_id, c->assistant_name, c->assistant_emoji,
                                                 cur ? cur->instructions : c->system));
    }
  gboolean removed = FALSE;
  for (guint i = 0; i < c->team->len && !removed; i++)
    if (g_strcmp0 (((TeamMember *) c->team->pdata[i])->id, id) == 0)
      {
        g_ptr_array_remove_index (c->team, i);
        removed = TRUE;
      }
  if (!removed)
    g_ptr_array_add (c->team, team_member_new (as->id, as->name, as->emoji, as->instructions));

  g_clear_pointer (&c->assistant_id, g_free);
  g_clear_pointer (&c->assistant_name, g_free);
  g_clear_pointer (&c->assistant_emoji, g_free);
  g_clear_pointer (&c->system, g_free);
  if (c->team->len == 1)
    {
      TeamMember *t = c->team->pdata[0];
      c->assistant_id = g_strdup (t->id);
      c->assistant_name = g_strdup (t->name);
      c->assistant_emoji = g_strdup (t->emoji);
      c->system = g_strdup (t->instructions);
      g_ptr_array_set_size (c->team, 0);
    }
  store_save (c);
  refresh_chat_list ();
  update_assistant_ui ();
}

static void
on_assistant_row_activated (GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
  (void) box;
  (void) user_data;
  const char *id = g_object_get_data (G_OBJECT (row), "id");
  if (A.team_mode && id)
    {
      toggle_team_member (id); /* keep the popover open to pick several */
      return;
    }
  gtk_menu_button_popdown (A.assistant_button);
  choose_assistant (id);
}

static void
on_team_switch (GObject *sw, GParamSpec *pspec, gpointer user_data)
{
  (void) pspec;
  (void) user_data;
  gboolean on = gtk_switch_get_active (GTK_SWITCH (sw));
  if (on == A.team_mode)
    return;
  A.team_mode = on;

  if (on)
    {
      /* Start the team with the current assistant. */
      if (!(A.conv && A.conv_saved) && A.pick_id && A.pick_team->len == 0)
        g_ptr_array_add (A.pick_team, g_strdup (A.pick_id));
    }
  else
    {
      /* Back to one assistant: keep the first member. */
      const char *first = NULL;
      if (A.conv && A.conv_saved && A.conv->team->len > 0)
        first = ((TeamMember *) A.conv->team->pdata[0])->id;
      else if (!(A.conv && A.conv_saved) && A.pick_team->len > 0)
        first = A.pick_team->pdata[0];
      else
        first = current_assistant_id ();
      g_autofree char *keep = g_strdup (first);
      g_ptr_array_set_size (A.pick_team, 0);
      choose_assistant (keep);
      return;
    }
  update_assistant_ui ();
}

static void
on_clear_assistant (GtkButton *button, gpointer user_data)
{
  (void) button;
  (void) user_data;
  A.team_mode = FALSE;
  g_ptr_array_set_size (A.pick_team, 0);
  choose_assistant (NULL);
}

static void
on_assistant_edit_clicked (GtkButton *button, gpointer user_data)
{
  (void) user_data;
  Assistant *as = assistants_find (A.assistants, g_object_get_data (G_OBJECT (button), "id"));
  gtk_menu_button_popdown (A.assistant_button);
  if (as)
    open_assistant_editor (as);
}

static void
on_new_assistant_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  (void) user_data;
  gtk_menu_button_popdown (A.assistant_button);
  open_assistant_editor (NULL);
}

static GtkWidget *
assistant_row (const char *id, const char *emoji, const char *name, const char *subtitle)
{
  GtkWidget *row = adw_action_row_new ();
  g_autofree char *title = g_strdup_printf ("%s  %s", emoji, name);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
  adw_action_row_set_subtitle (ADW_ACTION_ROW (row), subtitle);
  adw_action_row_set_subtitle_lines (ADW_ACTION_ROW (row), 1);
  gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), TRUE);
  g_object_set_data_full (G_OBJECT (row), "id", g_strdup (id), g_free);

  if (A.team_mode)
    {
      GtkWidget *box = gtk_check_button_new ();
      gtk_check_button_set_active (GTK_CHECK_BUTTON (box), in_current_team (id));
      gtk_widget_set_can_target (box, FALSE); /* the whole row toggles it */
      gtk_widget_set_valign (box, GTK_ALIGN_CENTER);
      adw_action_row_add_prefix (ADW_ACTION_ROW (row), box);
    }
  else
    {
      GtkWidget *check = gtk_image_new_from_icon_name ("object-select-symbolic");
      gtk_widget_add_css_class (check, "accent");
      gtk_widget_set_opacity (check, g_strcmp0 (id, current_assistant_id ()) == 0 ? 1 : 0);
      adw_action_row_add_suffix (ADW_ACTION_ROW (row), check);
    }

  if (id)
    {
      GtkWidget *edit = icon_button ("npu-edit-symbolic", TR ("Editar", "Edit"));
      g_object_set_data_full (G_OBJECT (edit), "id", g_strdup (id), g_free);
      g_signal_connect (edit, "clicked", G_CALLBACK (on_assistant_edit_clicked), NULL);
      adw_action_row_add_suffix (ADW_ACTION_ROW (row), edit);
    }
  return row;
}

/* Refreshes the header button, its list and the welcome page. */
static void
update_assistant_ui (void)
{
  if (!A.assistant_button)
    return;
  if (A.pick_id && !assistants_find (A.assistants, A.pick_id))
    g_clear_pointer (&A.pick_id, g_free);

  /* A chat may name an assistant that was deleted since: show its copy. */
  const char *emoji = "✦";
  const char *name = TR ("General", "General");
  if (A.conv && A.conv_saved && A.conv->assistant_name)
    {
      emoji = A.conv->assistant_emoji ? A.conv->assistant_emoji : "✦";
      name = A.conv->assistant_name;
    }
  else if (!(A.conv && A.conv_saved))
    {
      Assistant *as = assistants_find (A.assistants, A.pick_id);
      if (as)
        {
          emoji = as->emoji;
          name = as->name;
        }
    }
  g_autofree char *label = g_strdup_printf ("%s  %s", emoji, name);

  /* Teams show their members' emojis: "🏋️ 🍎  Team". */
  g_autoptr (GString) team = g_string_new (NULL);
  guint members = 0;
  if (A.conv && A.conv_saved)
    for (guint i = 0; i < A.conv->team->len; i++, members++)
      g_string_append_printf (team, "%s ", ((TeamMember *) A.conv->team->pdata[i])->emoji);
  else if (A.team_mode)
    for (guint i = 0; i < A.pick_team->len; i++)
      {
        Assistant *m = assistants_find (A.assistants, A.pick_team->pdata[i]);
        if (m)
          {
            g_string_append_printf (team, "%s ", m->emoji);
            members++;
          }
      }
  if (members >= 2)
    {
      g_string_append_printf (team, " %s", TR ("Equipo", "Team"));
      gtk_label_set_text (A.assistant_label, team->str);
    }
  else
    gtk_label_set_text (A.assistant_label, label);

  gboolean general = members < 2 && current_assistant_id () == NULL &&
                     !(A.team_mode && members == 1);
  gtk_widget_set_visible (A.clear_assistant, !general);
  gtk_switch_set_active (A.team_switch, A.team_mode);

  refresh_docs_ui ();

  gtk_list_box_remove_all (A.assistant_list);
  if (!A.team_mode)
    gtk_list_box_append (A.assistant_list, assistant_row (NULL, "✦", TR ("General", "General"),
                                                         TR ("Sin instrucciones especiales", "No special instructions")));
  for (guint i = 0; i < A.assistants->len; i++)
    {
      Assistant *as = A.assistants->pdata[i];
      gtk_list_box_append (A.assistant_list, assistant_row (as->id, as->emoji, as->name, as->instructions));
    }

  update_welcome_header ();
}

static GtkWidget *
build_assistant_button (void)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  A.assistant_label = GTK_LABEL (gtk_label_new (NULL));
  gtk_label_set_ellipsize (A.assistant_label, PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars (A.assistant_label, 18);
  gtk_widget_add_css_class (GTK_WIDGET (A.assistant_label), "heading");
  gtk_box_append (GTK_BOX (box), GTK_WIDGET (A.assistant_label));
  gtk_box_append (GTK_BOX (box), gtk_image_new_from_icon_name ("pan-down-symbolic"));

  GtkWidget *pop_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_add_css_class (pop_box, "picker");
  GtkWidget *title = dim_label (TR ("Asistente", "Assistant"), "caption-heading");
  gtk_widget_set_margin_start (title, 8);
  A.assistant_list = GTK_LIST_BOX (gtk_list_box_new ());
  gtk_list_box_set_selection_mode (A.assistant_list, GTK_SELECTION_NONE);
  gtk_widget_add_css_class (GTK_WIDGET (A.assistant_list), "boxed-list");
  g_signal_connect (A.assistant_list, "row-activated", G_CALLBACK (on_assistant_row_activated), NULL);
  GtkWidget *sw = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (sw), TRUE);
  gtk_scrolled_window_set_max_content_height (GTK_SCROLLED_WINDOW (sw), 420);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (sw), GTK_WIDGET (A.assistant_list));

  GtkWidget *explore = gtk_button_new_with_label (TR ("Explorar galería", "Browse gallery"));
  gtk_widget_add_css_class (explore, "flat");
  gtk_widget_set_hexpand (explore, TRUE);
  g_signal_connect (explore, "clicked", G_CALLBACK (on_gallery_clicked), NULL);
  GtkWidget *add = gtk_button_new_with_label (TR ("+  Crear propio", "+  Create your own"));
  gtk_widget_add_css_class (add, "flat");
  gtk_widget_set_hexpand (add, TRUE);
  g_signal_connect (add, "clicked", G_CALLBACK (on_new_assistant_clicked), NULL);
  GtkWidget *actions = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_set_homogeneous (GTK_BOX (actions), TRUE);
  gtk_box_append (GTK_BOX (actions), explore);
  gtk_box_append (GTK_BOX (actions), add);

  GtkWidget *team_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_add_css_class (team_row, "team-row");
  GtkWidget *team_text = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_hexpand (team_text, TRUE);
  GtkWidget *team_title = gtk_label_new (TR ("Modo equipo", "Team mode"));
  gtk_label_set_xalign (GTK_LABEL (team_title), 0);
  gtk_widget_add_css_class (team_title, "heading");
  GtkWidget *team_sub = dim_label (TR ("Elige varios: responde el especialista de cada tema",
                                       "Pick several: the right specialist answers each topic"), "caption");
  gtk_box_append (GTK_BOX (team_text), team_title);
  gtk_box_append (GTK_BOX (team_text), team_sub);
  A.team_switch = GTK_SWITCH (gtk_switch_new ());
  gtk_widget_set_valign (GTK_WIDGET (A.team_switch), GTK_ALIGN_CENTER);
  g_signal_connect (A.team_switch, "notify::active", G_CALLBACK (on_team_switch), NULL);
  gtk_box_append (GTK_BOX (team_row), team_text);
  gtk_box_append (GTK_BOX (team_row), GTK_WIDGET (A.team_switch));

  gtk_box_append (GTK_BOX (pop_box), title);
  gtk_box_append (GTK_BOX (pop_box), team_row);
  gtk_box_append (GTK_BOX (pop_box), sw);
  gtk_box_append (GTK_BOX (pop_box), actions);

  GtkWidget *popover = gtk_popover_new ();
  gtk_widget_set_size_request (popover, 360, -1);
  gtk_popover_set_child (GTK_POPOVER (popover), pop_box);

  A.assistant_button = GTK_MENU_BUTTON (gtk_menu_button_new ());
  gtk_menu_button_set_child (A.assistant_button, box);
  gtk_menu_button_set_popover (A.assistant_button, popover);
  gtk_widget_add_css_class (GTK_WIDGET (A.assistant_button), "flat");
  gtk_widget_add_css_class (GTK_WIDGET (A.assistant_button), "model-button");
  gtk_widget_set_tooltip_text (GTK_WIDGET (A.assistant_button), TR ("Asistente", "Assistant"));
  gtk_widget_set_valign (GTK_WIDGET (A.assistant_button), GTK_ALIGN_CENTER);

  /* × next to the selector: back to General in one click. */
  A.clear_assistant = icon_button ("window-close-symbolic", TR ("Quitar asistente (volver a General)",
                                                                "Remove assistant (back to General)"));
  gtk_widget_add_css_class (A.clear_assistant, "clear-assistant");
  g_signal_connect (A.clear_assistant, "clicked", G_CALLBACK (on_clear_assistant), NULL);

  GtkWidget *wrap = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_valign (wrap, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (wrap), GTK_WIDGET (A.assistant_button));
  gtk_box_append (GTK_BOX (wrap), A.clear_assistant);
  return wrap;
}

static char *
text_view_contents (GtkTextView *view)
{
  GtkTextBuffer *buf = gtk_text_view_get_buffer (view);
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds (buf, &start, &end);
  return g_strstrip (gtk_text_buffer_get_text (buf, &start, &end, FALSE));
}

static void
on_editor_save (GtkButton *button, gpointer user_data)
{
  (void) button;
  Editor *e = user_data;
  g_autofree char *name = g_strstrip (g_strdup (gtk_editable_get_text (GTK_EDITABLE (e->name))));
  g_autofree char *emoji = g_strstrip (g_strdup (gtk_editable_get_text (GTK_EDITABLE (e->emoji))));
  g_autofree char *instructions = text_view_contents (e->instructions);

  if (!*name)
    {
      gtk_widget_add_css_class (GTK_WIDGET (e->name), "error");
      gtk_widget_grab_focus (GTK_WIDGET (e->name));
      return;
    }

  Assistant *a = assistants_find (A.assistants, e->id);
  gboolean created = a == NULL;
  g_autoptr (GPtrArray) chosen = g_ptr_array_new_with_free_func (g_free);
  for (GtkWidget *row = gtk_widget_get_first_child (GTK_WIDGET (e->libs)); row; row = gtk_widget_get_next_sibling (row))
    {
      GtkWidget *check = g_object_get_data (G_OBJECT (row), "check");
      if (check && gtk_check_button_get_active (GTK_CHECK_BUTTON (check)))
        g_ptr_array_add (chosen, g_strdup (g_object_get_data (G_OBJECT (row), "id")));
    }
  if (a)
    {
      g_free (a->name);
      g_free (a->emoji);
      g_free (a->instructions);
      a->name = g_steal_pointer (&name);
      a->emoji = g_strdup (*emoji ? emoji : "✦");
      a->instructions = g_steal_pointer (&instructions);
    }
  else
    {
      a = assistant_new (name, *emoji ? emoji : "✦", instructions);
      g_ptr_array_add (A.assistants, a);
    }
  g_ptr_array_set_size (a->libs, 0);
  for (guint i = 0; i < chosen->len; i++)
    g_ptr_array_add (a->libs, g_strdup (chosen->pdata[i]));
  assistants_save (A.assistants);
  drop_rag_index ();
  refresh_docs_ui ();

  /* A new assistant starts a fresh chat; an edited one also applies to the
   * open chat if that chat uses it. Other chats keep their own copy. */
  if (created && A.conv && A.conv_saved)
    {
      g_free (A.pick_id);
      A.pick_id = g_strdup (a->id);
      new_chat ();
    }
  else if (created || (A.conv && A.conv_saved && g_strcmp0 (A.conv->assistant_id, a->id) == 0))
    choose_assistant (a->id);
  else
    update_assistant_ui ();
  adw_dialog_close (e->dialog);
}

static void
on_delete_assistant_response (AdwAlertDialog *dialog, const char *response, gpointer user_data)
{
  (void) dialog;
  const char *id = user_data;
  Assistant *a = assistants_find (A.assistants, id);
  if (!g_str_equal (response, "delete") || !a)
    return;
  g_ptr_array_remove (A.assistants, a);
  assistants_save (A.assistants);
  update_assistant_ui ();
  if (editor_current)
    adw_dialog_close (editor_current->dialog);
}

static void
on_editor_delete (GtkButton *button, gpointer user_data)
{
  (void) button;
  Editor *e = user_data;
  Assistant *a = assistants_find (A.assistants, e->id);
  if (!a)
    return;
  AdwDialog *d = TRACK (adw_alert_dialog_new (NULL, NULL));
  adw_alert_dialog_format_heading (ADW_ALERT_DIALOG (d), TR ("¿Eliminar «%s»?", "Delete “%s”?"), a->name);
  adw_alert_dialog_set_body (ADW_ALERT_DIALOG (d), TR ("Tus conversaciones con este asistente se conservan.",
                                                       "Your chats with this assistant are kept."));
  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (d), "cancel", TR ("Cancelar", "Cancel"),
                                  "delete", TR ("Eliminar", "Delete"), NULL);
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (d), "delete", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (d), "cancel");
  g_signal_connect_data (d, "response", G_CALLBACK (on_delete_assistant_response),
                         g_strdup (e->id), free_closure_string, 0);
  adw_dialog_present (d, GTK_WIDGET (A.win));
}

static void
on_editor_closed (AdwDialog *dialog, gpointer user_data)
{
  (void) dialog;
  Editor *e = user_data;
  if (editor_current == e)
    editor_current = NULL;
  g_free (e->id);
  g_free (e);
}

static void
on_editor_cancel (GtkButton *button, gpointer user_data)
{
  (void) button;
  adw_dialog_close (((Editor *) user_data)->dialog);
}

static void
open_assistant_editor (Assistant *a)
{
  if (editor_current)
    adw_dialog_force_close (editor_current->dialog);

  Editor *e = g_new0 (Editor, 1);
  e->id = a ? g_strdup (a->id) : NULL;
  editor_current = e;

  e->dialog = TRACK (adw_dialog_new ());
  adw_dialog_set_title (e->dialog, a ? TR ("Editar asistente", "Edit assistant") : TR ("Nuevo asistente", "New assistant"));
  adw_dialog_set_content_width (e->dialog, 540);
  adw_dialog_set_content_height (e->dialog, 640);

  GtkWidget *tv = adw_toolbar_view_new ();
  GtkWidget *header = adw_header_bar_new ();
  adw_header_bar_set_show_end_title_buttons (ADW_HEADER_BAR (header), FALSE);
  adw_header_bar_set_show_start_title_buttons (ADW_HEADER_BAR (header), FALSE);
  GtkWidget *cancel = gtk_button_new_with_label (TR ("Cancelar", "Cancel"));
  g_signal_connect (cancel, "clicked", G_CALLBACK (on_editor_cancel), e);
  GtkWidget *save = gtk_button_new_with_label (TR ("Guardar", "Save"));
  gtk_widget_add_css_class (save, "suggested-action");
  g_signal_connect (save, "clicked", G_CALLBACK (on_editor_save), e);
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), cancel);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), save);
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (tv), header);

  GtkWidget *page = adw_preferences_page_new ();

  AdwPreferencesGroup *who = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  e->name = ADW_ENTRY_ROW (adw_entry_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (e->name), TR ("Nombre", "Name"));
  gtk_editable_set_text (GTK_EDITABLE (e->name), a ? a->name : "");
  e->emoji = ADW_ENTRY_ROW (adw_entry_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (e->emoji), TR ("Emoji (ej. 🧠 ♟️ 💡 🍎)", "Emoji (e.g. 🧠 ♟️ 💡 🍎)"));
  gtk_editable_set_text (GTK_EDITABLE (e->emoji), a ? a->emoji : "");
  adw_preferences_group_add (who, GTK_WIDGET (e->name));
  adw_preferences_group_add (who, GTK_WIDGET (e->emoji));

  AdwPreferencesGroup *how = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (how, TR ("Instrucciones", "Instructions"));
  adw_preferences_group_set_description (how, TR ("Describe su rol, su tono y lo que debe evitar. "
                                                  "Ej.: «Eres un nutricionista amable que da planes sencillos…»",
                                                  "Describe its role, tone and what to avoid. "
                                                  "E.g. “You are a friendly nutritionist who gives simple plans…”"));
  e->instructions = GTK_TEXT_VIEW (gtk_text_view_new ());
  gtk_text_view_set_wrap_mode (e->instructions, GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_accepts_tab (e->instructions, FALSE);
  gtk_text_buffer_set_text (gtk_text_view_get_buffer (e->instructions), a ? a->instructions : "", -1);
  GtkWidget *sw = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_min_content_height (GTK_SCROLLED_WINDOW (sw), 240);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (sw), GTK_WIDGET (e->instructions));
  gtk_widget_add_css_class (sw, "card");
  gtk_widget_add_css_class (sw, "instructions");
  adw_preferences_group_add (how, sw);

  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), who);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), how);

  AdwPreferencesGroup *docs = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (docs, TR ("Documentos", "Documents"));
  adw_preferences_group_set_description (docs, TR ("Este asistente siempre responderá usando las bibliotecas marcadas.",
                                                   "This assistant will always answer using the ticked libraries."));
  e->libs = GTK_LIST_BOX (gtk_list_box_new ());
  gtk_list_box_set_selection_mode (e->libs, GTK_SELECTION_NONE);
  gtk_widget_add_css_class (GTK_WIDGET (e->libs), "boxed-list");
  GtkWidget *ph = dim_label (TR ("Crea una biblioteca desde el clip del chat para usarla aquí.",
                                 "Create a library from the chat's paperclip to use it here."), "placeholder-row");
  gtk_label_set_xalign (GTK_LABEL (ph), 0.5);
  gtk_list_box_set_placeholder (e->libs, ph);
  for (guint i = 0; i < A.libraries->len; i++)
    {
      RagLibrary *lib = A.libraries->pdata[i];
      if (lib->chat_scoped)
        continue;
      GtkWidget *row = adw_action_row_new ();
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), lib->name);
      adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
      g_autofree char *sub = library_subtitle (lib);
      adw_action_row_set_subtitle (ADW_ACTION_ROW (row), sub);
      GtkWidget *check = gtk_check_button_new ();
      gboolean on = FALSE;
      for (guint k = 0; a && a->libs && k < a->libs->len; k++)
        on |= g_str_equal (a->libs->pdata[k], lib->id);
      gtk_check_button_set_active (GTK_CHECK_BUTTON (check), on);
      gtk_widget_set_valign (check, GTK_ALIGN_CENTER);
      adw_action_row_add_prefix (ADW_ACTION_ROW (row), check);
      adw_action_row_set_activatable_widget (ADW_ACTION_ROW (row), check);
      g_object_set_data (G_OBJECT (row), "check", check);
      g_object_set_data_full (G_OBJECT (row), "id", g_strdup (lib->id), g_free);
      gtk_list_box_append (e->libs, row);
    }
  adw_preferences_group_add (docs, GTK_WIDGET (e->libs));
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), docs);

  if (a)
    {
      AdwPreferencesGroup *danger = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
      GtkWidget *del = gtk_button_new_with_label (TR ("Eliminar asistente", "Delete assistant"));
      gtk_widget_add_css_class (del, "destructive-action");
      gtk_widget_add_css_class (del, "pill");
      gtk_widget_set_halign (del, GTK_ALIGN_CENTER);
      g_signal_connect (del, "clicked", G_CALLBACK (on_editor_delete), e);
      adw_preferences_group_add (danger, del);
      adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), danger);
    }

  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (tv), page);
  adw_dialog_set_child (e->dialog, tv);
  adw_dialog_set_default_widget (e->dialog, save);
  g_signal_connect (e->dialog, "closed", G_CALLBACK (on_editor_closed), e);
  adw_dialog_present (e->dialog, GTK_WIDGET (A.win));
  gtk_widget_grab_focus (GTK_WIDGET (a ? GTK_WIDGET (e->instructions) : GTK_WIDGET (e->name)));
}

static void
act_new_assistant (GSimpleAction *a, GVariant *p, gpointer d)
{
  (void) a; (void) p; (void) d;
  open_assistant_editor (NULL);
}

/* ---- assistant gallery ------------------------------------------------ */

typedef struct {
  AdwDialog *dialog;
  GtkWidget *groups[CAT_COUNT];
  GPtrArray *rows; /* rows, owned by the dialog */
  GtkWidget *no_results;
  GtkWidget *search;
} Gallery;

static Gallery gallery;

/* Added templates offer "Remove"; the others offer "Add". */
static void
gallery_mark_added (GtkWidget *button, gboolean added)
{
  gtk_button_set_label (GTK_BUTTON (button), added ? TR ("Quitar", "Remove") : TR ("Añadir", "Add"));
  if (added)
    {
      gtk_widget_remove_css_class (button, "suggested-action");
      gtk_widget_set_tooltip_text (button, TR ("Quitar de tu lista (tus chats se conservan)",
                                               "Remove from your list (your chats are kept)"));
    }
  else
    {
      gtk_widget_add_css_class (button, "suggested-action");
      gtk_widget_set_tooltip_text (button, NULL);
    }
}

static void
gallery_sync_button (const char *key, gboolean added)
{
  for (guint i = 0; gallery.rows && i < gallery.rows->len; i++)
    {
      GObject *row = gallery.rows->pdata[i];
      const AssistantTemplate *t = g_object_get_data (row, "template");
      if (g_str_equal (t->key, key))
        gallery_mark_added (g_object_get_data (row, "button"), added);
    }
}

static void
gallery_remove (const char *key)
{
  for (guint i = 0; i < A.assistants->len; i++)
    {
      Assistant *a = A.assistants->pdata[i];
      if (g_strcmp0 (a->template_key, key) != 0)
        continue;
      toast (TR ("%s %s quitado de tu lista", "%s %s removed from your list"), a->emoji, a->name);
      for (guint j = 0; j < A.pick_team->len; j++)
        if (g_str_equal (A.pick_team->pdata[j], a->id))
          g_ptr_array_remove_index (A.pick_team, j--);
      g_ptr_array_remove_index (A.assistants, i);
      break;
    }
  assistants_save (A.assistants);
  gallery_sync_button (key, FALSE);
  update_assistant_ui ();
}

static Assistant *
gallery_add (const char *key)
{
  if (assistants_has_template (A.assistants, key))
    return NULL;
  Assistant *a = assistant_new_from_template (key);
  if (!a)
    return NULL;
  gallery_sync_button (key, TRUE);
  g_ptr_array_add (A.assistants, a);
  assistants_save (A.assistants);
  update_assistant_ui ();
  toast (TR ("%s %s añadido. Elígelo en el selector de arriba.", "%s %s added. Pick it from the selector at the top."),
         a->emoji, a->name);
  return a;
}

static void
on_gallery_add (GtkButton *button, gpointer user_data)
{
  (void) user_data;
  const char *key = g_object_get_data (G_OBJECT (button), "key");
  if (assistants_has_template (A.assistants, key))
    gallery_remove (key);
  else
    gallery_add (key);
}

static gboolean
contains_casefold (const char *haystack, const char *needle_folded)
{
  g_autofree char *h = g_utf8_casefold (haystack, -1);
  return strstr (h, needle_folded) != NULL;
}

static void
on_gallery_search (GtkSearchEntry *entry, gpointer user_data)
{
  (void) user_data;
  g_autofree char *q = g_utf8_casefold (gtk_editable_get_text (GTK_EDITABLE (entry)), -1);
  guint per_cat[CAT_COUNT] = { 0 };
  guint total = 0;

  for (guint i = 0; i < gallery.rows->len; i++)
    {
      GtkWidget *row = gallery.rows->pdata[i];
      const AssistantTemplate *t = g_object_get_data (G_OBJECT (row), "template");
      /* Match both languages, so "chef" or "cocina" both work. */
      gboolean match = !*q || contains_casefold (t->name_es, q) || contains_casefold (t->name_en, q) ||
                       contains_casefold (t->desc_es, q) || contains_casefold (t->desc_en, q);
      gtk_widget_set_visible (row, match);
      if (match)
        {
          per_cat[t->category]++;
          total++;
        }
    }
  for (int c = 0; c < CAT_COUNT; c++)
    gtk_widget_set_visible (gallery.groups[c], per_cat[c] > 0);
  gtk_widget_set_visible (gallery.no_results, total == 0);
}

static void
on_gallery_closed (AdwDialog *dialog, gpointer user_data)
{
  (void) dialog;
  (void) user_data;
  g_clear_pointer (&gallery.rows, g_ptr_array_unref);
  gallery.dialog = NULL;
}

static void
open_gallery (void)
{
  if (gallery.dialog)
    return;

  gallery.dialog = TRACK (adw_dialog_new ());
  adw_dialog_set_title (gallery.dialog, TR ("Galería de asistentes", "Assistant gallery"));
  adw_dialog_set_content_width (gallery.dialog, 600);
  adw_dialog_set_content_height (gallery.dialog, 720);
  gallery.rows = g_ptr_array_new ();

  GtkWidget *tv = adw_toolbar_view_new ();
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (tv), adw_header_bar_new ());
  GtkWidget *search = gtk_search_entry_new ();
  gtk_search_entry_set_placeholder_text (GTK_SEARCH_ENTRY (search), TR ("Buscar (ej. cocina, inglés, código)",
                                                                       "Search (e.g. cooking, English, code)"));
  gtk_widget_set_margin_start (search, 12);
  gtk_widget_set_margin_end (search, 12);
  gtk_widget_set_margin_bottom (search, 6);
  g_signal_connect (search, "search-changed", G_CALLBACK (on_gallery_search), NULL);
  gallery.search = search;
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (tv), search);

  GtkWidget *page = adw_preferences_page_new ();
  adw_preferences_page_set_description (ADW_PREFERENCES_PAGE (page),
                                        TR ("Añade los que quieras; aparecerán en el selector de asistente. "
                                            "Después puedes editarlos a tu gusto.",
                                            "Add the ones you like; they appear in the assistant selector. "
                                            "You can edit them afterwards."));
  for (int c = 0; c < CAT_COUNT; c++)
    {
      gallery.groups[c] = adw_preferences_group_new ();
      adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (gallery.groups[c]), template_category_name (c));
      adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), ADW_PREFERENCES_GROUP (gallery.groups[c]));
    }

  for (guint i = 0; i < assistant_templates_count; i++)
    {
      const AssistantTemplate *t = &assistant_templates[i];
      GtkWidget *row = adw_action_row_new ();
      g_autofree char *title = g_strdup_printf ("%s  %s", t->emoji, TR (t->name_es, t->name_en));
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
      adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
      adw_action_row_set_subtitle (ADW_ACTION_ROW (row), TR (t->desc_es, t->desc_en));
      g_object_set_data (G_OBJECT (row), "template", (gpointer) t);

      GtkWidget *add = gtk_button_new_with_label (TR ("Añadir", "Add"));
      gtk_widget_set_valign (add, GTK_ALIGN_CENTER);
      gtk_widget_add_css_class (add, "suggested-action");
      g_object_set_data (G_OBJECT (add), "key", (gpointer) t->key);
      g_signal_connect (add, "clicked", G_CALLBACK (on_gallery_add), NULL);
      g_object_set_data (G_OBJECT (row), "button", add);
      gallery_mark_added (add, assistants_has_template (A.assistants, t->key));
      adw_action_row_add_suffix (ADW_ACTION_ROW (row), add);

      adw_preferences_group_add (ADW_PREFERENCES_GROUP (gallery.groups[t->category]), row);
      g_ptr_array_add (gallery.rows, row);
    }

  AdwPreferencesGroup *none = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  GtkWidget *msg = dim_label (TR ("No hay asistentes con ese nombre. Puedes crear el tuyo con «+ Nuevo asistente».",
                                  "No assistant matches. You can create your own with “+ New assistant”."), NULL);
  gtk_label_set_xalign (GTK_LABEL (msg), 0.5);
  gtk_label_set_justify (GTK_LABEL (msg), GTK_JUSTIFY_CENTER);
  adw_preferences_group_add (none, msg);
  gallery.no_results = GTK_WIDGET (none);
  gtk_widget_set_visible (gallery.no_results, FALSE);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), none);

  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (tv), page);
  adw_dialog_set_child (gallery.dialog, tv);
  g_signal_connect (gallery.dialog, "closed", G_CALLBACK (on_gallery_closed), NULL);
  adw_dialog_present (gallery.dialog, GTK_WIDGET (A.win));
  gtk_widget_grab_focus (search);
}

static void
act_gallery (GSimpleAction *a, GVariant *p, gpointer d)
{
  (void) a; (void) p; (void) d;
  open_gallery ();
}

static void
on_gallery_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  (void) user_data;
  gtk_menu_button_popdown (A.assistant_button);
  open_gallery ();
}

/* ---- documents UI ------------------------------------------------------ */

static void
refresh_all_docs_ui (void)
{
  refresh_docs_ui ();
  if (A.libs_dialog)
    libs_dialog_refresh ();
}

static gboolean
lib_is_from_assistant (const char *id)
{
  Assistant *as = assistants_find (A.assistants, A.conv && A.conv_saved ? A.conv->assistant_id : A.pick_id);
  for (guint i = 0; as && as->libs && i < as->libs->len; i++)
    if (g_str_equal (as->libs->pdata[i], id))
      return TRUE;
  return FALSE;
}

static void
conv_libs_changed (void)
{
  if (A.conv && A.conv_saved)
    store_save (A.conv);
  drop_rag_index ();
  refresh_all_docs_ui ();
}

static char *
library_subtitle (const RagLibrary *lib)
{
  if (lib->busy)
    return g_strdup (TR ("Importando…", "Importing…"));
  guint files = lib->docs->len, passages = rag_library_chunk_count (lib);
  g_autofree char *f = files == 1 ? g_strdup (TR ("1 archivo", "1 file"))
                                  : g_strdup_printf (TR ("%u archivos", "%u files"), files);
  g_autofree char *p = passages == 1 ? g_strdup (TR ("1 fragmento", "1 passage"))
                                     : g_strdup_printf (TR ("%u fragmentos", "%u passages"), passages);
  return g_strdup_printf ("%s · %s", f, p);
}

static void
on_doc_row_activated (GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
  (void) box;
  (void) user_data;
  const char *id = g_object_get_data (G_OBJECT (row), "id");
  if (!A.conv || !id || lib_is_from_assistant (id))
    return;
  conversation_toggle_lib (A.conv, id);
  conv_libs_changed ();
}

/* The popover's list: libraries the chat can use, ticked when attached. */
static void
refresh_docs_ui (void)
{
  if (!A.docs_list)
    return;
  gtk_list_box_remove_all (A.docs_list);

  g_autoptr (GPtrArray) active = effective_libs ();
  for (guint i = 0; A.libraries && i < A.libraries->len; i++)
    {
      RagLibrary *lib = A.libraries->pdata[i];
      gboolean attached = A.conv && conversation_has_lib (A.conv, lib->id);
      /* Another chat's loose files are not offered here. */
      if (lib->chat_scoped && !attached)
        continue;
      gboolean from_assistant = lib_is_from_assistant (lib->id);

      GtkWidget *row = adw_action_row_new ();
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), lib->name);
      adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
      g_autofree char *sub = library_subtitle (lib);
      g_autofree char *full = from_assistant ? g_strdup_printf ("%s · %s", sub, TR ("del asistente", "from the assistant"))
                                             : g_strdup (sub);
      adw_action_row_set_subtitle (ADW_ACTION_ROW (row), full);
      gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), !from_assistant);

      GtkWidget *check = gtk_check_button_new ();
      gtk_check_button_set_active (GTK_CHECK_BUTTON (check), attached || from_assistant);
      gtk_widget_set_can_target (check, FALSE);
      gtk_widget_set_sensitive (check, !from_assistant);
      gtk_widget_set_valign (check, GTK_ALIGN_CENTER);
      adw_action_row_add_prefix (ADW_ACTION_ROW (row), check);
      g_object_set_data_full (G_OBJECT (row), "id", g_strdup (lib->id), g_free);
      gtk_list_box_append (A.docs_list, row);
    }

  if (A.docs_button)
    {
      g_autofree char *tip = active->len ? g_strdup_printf (TR ("Documentos (%u en uso)", "Documents (%u in use)"), active->len)
                                         : g_strdup (TR ("Documentos", "Documents"));
      gtk_widget_set_tooltip_text (GTK_WIDGET (A.docs_button), tip);
      if (active->len)
        gtk_widget_add_css_class (GTK_WIDGET (A.docs_button), "has-docs");
      else
        gtk_widget_remove_css_class (GTK_WIDGET (A.docs_button), "has-docs");
    }
}

/* ---- importing files ---- */

typedef struct {
  char *lib_id;
} ImportUi;

static void
on_import_progress (double fraction, const char *file, gpointer user_data)
{
  (void) user_data;
  if (A.quitting || !A.import_bar)
    return;
  gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (A.import_bar), fraction);
  if (file)
    {
      g_autofree char *text = g_strdup_printf (TR ("Leyendo %s…", "Reading %s…"), file);
      gtk_label_set_text (A.import_label, text);
    }
}

static gboolean
hide_import_bar (gpointer user_data)
{
  (void) user_data;
  if (A.import_revealer && !A.quitting)
    gtk_revealer_set_reveal_child (GTK_REVEALER (A.import_revealer), FALSE);
  return G_SOURCE_REMOVE;
}

static void
on_import_done (guint added, char **errors, gpointer user_data)
{
  ImportUi *ui = user_data;
  g_free (ui->lib_id);
  g_free (ui);
  if (A.quitting)
    return;

  if (added > 0)
    toast (added == 1 ? TR ("1 archivo listo para consultar", "1 file ready to search")
                      : TR ("%u archivos listos para consultar", "%u files ready to search"), added);
  if (errors && errors[0])
    toast ("%s", errors[0]); /* the first one is enough; the rest are in the library view */
  gtk_label_set_text (A.import_label, added ? TR ("Listo", "Done") : TR ("No se pudo importar", "Import failed"));
  gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (A.import_bar), 1.0);
  g_timeout_add_seconds (2, hide_import_bar, NULL);
  drop_rag_index ();
  refresh_all_docs_ui ();
}

static void
start_import (RagLibrary *lib, char **paths)
{
  if (lib->busy)
    {
      toast ("%s", TR ("Esa biblioteca todavía se está importando. Espera un momento.",
                       "That library is still importing. Wait a moment."));
      return;
    }
  ImportUi *ui = g_new0 (ImportUi, 1);
  ui->lib_id = g_strdup (lib->id);
  gtk_label_set_text (A.import_label, TR ("Preparando…", "Preparing…"));
  gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (A.import_bar), 0);
  gtk_revealer_set_reveal_child (GTK_REVEALER (A.import_revealer), TRUE);
  rag_library_add_files (lib, paths, on_import_progress, on_import_done, ui);
  refresh_all_docs_ui ();
}

/* Files dropped into (or added from) a chat go into that chat's own library. */
static RagLibrary *
chat_library (void)
{
  for (guint i = 0; A.conv && i < A.conv->libs->len; i++)
    {
      RagLibrary *lib = rag_library_find (A.libraries, A.conv->libs->pdata[i]);
      if (lib && lib->chat_scoped)
        return lib;
    }
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  g_autofree char *when = g_date_time_format (now, "%d/%m %H:%M");
  g_autofree char *name = g_strdup_printf ("%s · %s", TR ("Archivos del chat", "Chat files"), when);
  RagLibrary *lib = rag_library_new (name, TRUE);
  g_ptr_array_add (A.libraries, lib);
  conversation_toggle_lib (A.conv, lib->id);
  if (A.conv_saved)
    store_save (A.conv);
  return lib;
}

static void
import_into_chat (char **paths)
{
  g_autoptr (GPtrArray) ok = g_ptr_array_new ();
  for (guint i = 0; paths[i]; i++)
    if (rag_supported_file (paths[i]))
      g_ptr_array_add (ok, paths[i]);
  if (ok->len == 0)
    {
      toast ("%s", rag_pdf_available ()
                     ? TR ("Formato no compatible. Usa PDF, texto o Markdown.", "Unsupported format. Use PDF, text or Markdown.")
                     : TR ("Formato no compatible. Usa texto o Markdown.", "Unsupported format. Use text or Markdown."));
      return;
    }
  g_ptr_array_add (ok, NULL);
  start_import (chat_library (), (char **) ok->pdata);
}

static void
on_files_chosen (GObject *src, GAsyncResult *res, gpointer user_data)
{
  RagLibrary *lib = user_data; /* NULL: the chat's own library */
  g_autoptr (GListModel) files = gtk_file_dialog_open_multiple_finish (GTK_FILE_DIALOG (src), res, NULL);
  if (!files || A.quitting)
    return;

  g_autoptr (GPtrArray) paths = g_ptr_array_new_with_free_func (g_free);
  for (guint i = 0; i < g_list_model_get_n_items (files); i++)
    {
      g_autoptr (GFile) f = g_list_model_get_item (files, i);
      char *path = g_file_get_path (f);
      if (path)
        g_ptr_array_add (paths, path);
    }
  g_ptr_array_add (paths, NULL);
  if (lib && rag_library_find (A.libraries, lib->id) == lib)
    start_import (lib, (char **) paths->pdata);
  else
    import_into_chat ((char **) paths->pdata);
}

static void
choose_files (RagLibrary *lib)
{
  GtkFileDialog *dialog = gtk_file_dialog_new ();
  gtk_file_dialog_set_title (dialog, TR ("Añadir archivos", "Add files"));

  GtkFileFilter *filter = gtk_file_filter_new ();
  gtk_file_filter_set_name (filter, rag_pdf_available () ? TR ("Documentos (PDF, texto, Markdown)", "Documents (PDF, text, Markdown)")
                                                        : TR ("Documentos (texto, Markdown)", "Documents (text, Markdown)"));
  static const char *patterns[] = { "txt", "md", "markdown", "rst", "org", "csv", "tsv", "json", "yaml", "yml", "toml",
                                    "ini", "log", "c", "h", "cpp", "hpp", "py", "js", "ts", "rs", "go", "java", "sh",
                                    "html", "xml", "tex", "pdf" };
  for (guint i = 0; i < G_N_ELEMENTS (patterns); i++)
    {
      if (g_str_equal (patterns[i], "pdf") && !rag_pdf_available ())
        continue;
      g_autofree char *lower = g_strdup_printf ("*.%s", patterns[i]);
      g_autofree char *upper = g_ascii_strup (lower, -1);
      gtk_file_filter_add_pattern (filter, lower);
      gtk_file_filter_add_pattern (filter, upper);
    }
  g_autoptr (GListStore) filters = g_list_store_new (GTK_TYPE_FILE_FILTER);
  g_list_store_append (filters, filter);
  gtk_file_dialog_set_filters (dialog, G_LIST_MODEL (filters));
  gtk_file_dialog_set_default_filter (dialog, filter);
  g_object_unref (filter);

  gtk_file_dialog_open_multiple (dialog, A.win, NULL, on_files_chosen, lib);
  g_object_unref (dialog);
}

static void
on_add_files_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  (void) user_data;
  gtk_menu_button_popdown (A.docs_button);
  choose_files (NULL);
}

static void
on_manage_libs_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  (void) user_data;
  gtk_menu_button_popdown (A.docs_button);
  open_libraries_dialog ();
}

static gboolean
on_files_dropped (GtkDropTarget *target, const GValue *value, double x, double y, gpointer user_data)
{
  (void) target;
  (void) x;
  (void) y;
  (void) user_data;
  GdkFileList *list = g_value_get_boxed (value);
  if (!list || !A.conv)
    return FALSE;

  g_autoptr (GPtrArray) paths = g_ptr_array_new_with_free_func (g_free);
  for (GSList *l = gdk_file_list_get_files (list); l; l = l->next)
    {
      char *path = g_file_get_path (l->data);
      if (path)
        g_ptr_array_add (paths, path);
    }
  g_ptr_array_add (paths, NULL);
  import_into_chat ((char **) paths->pdata);
  return TRUE;
}

static GtkWidget *
build_docs_button (void)
{
  GtkWidget *pop_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_add_css_class (pop_box, "picker");
  GtkWidget *title = dim_label (TR ("Documentos", "Documents"), "caption-heading");
  gtk_widget_set_margin_start (title, 8);
  GtkWidget *hint = dim_label (TR ("Las respuestas se basarán en los archivos marcados.",
                                   "Answers will be based on the ticked files."), "caption");
  gtk_widget_set_margin_start (hint, 8);

  A.docs_list = GTK_LIST_BOX (gtk_list_box_new ());
  gtk_list_box_set_selection_mode (A.docs_list, GTK_SELECTION_NONE);
  gtk_widget_add_css_class (GTK_WIDGET (A.docs_list), "boxed-list");
  GtkWidget *ph = dim_label (TR ("Aún no hay documentos. Añade archivos o arrástralos al chat.",
                                 "No documents yet. Add files or drop them into the chat."), "placeholder-row");
  gtk_label_set_xalign (GTK_LABEL (ph), 0.5);
  gtk_label_set_justify (GTK_LABEL (ph), GTK_JUSTIFY_CENTER);
  gtk_list_box_set_placeholder (A.docs_list, ph);
  g_signal_connect (A.docs_list, "row-activated", G_CALLBACK (on_doc_row_activated), NULL);
  GtkWidget *sw = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (sw), TRUE);
  gtk_scrolled_window_set_max_content_height (GTK_SCROLLED_WINDOW (sw), 300);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (sw), GTK_WIDGET (A.docs_list));

  GtkWidget *add = gtk_button_new_with_label (TR ("Añadir archivos…", "Add files…"));
  gtk_widget_add_css_class (add, "flat");
  gtk_widget_set_hexpand (add, TRUE);
  g_signal_connect (add, "clicked", G_CALLBACK (on_add_files_clicked), NULL);
  GtkWidget *manage = gtk_button_new_with_label (TR ("Bibliotecas…", "Libraries…"));
  gtk_widget_add_css_class (manage, "flat");
  gtk_widget_set_hexpand (manage, TRUE);
  g_signal_connect (manage, "clicked", G_CALLBACK (on_manage_libs_clicked), NULL);
  GtkWidget *actions = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_set_homogeneous (GTK_BOX (actions), TRUE);
  gtk_box_append (GTK_BOX (actions), add);
  gtk_box_append (GTK_BOX (actions), manage);

  gtk_box_append (GTK_BOX (pop_box), title);
  gtk_box_append (GTK_BOX (pop_box), hint);
  gtk_box_append (GTK_BOX (pop_box), sw);
  gtk_box_append (GTK_BOX (pop_box), actions);

  GtkWidget *popover = gtk_popover_new ();
  gtk_widget_set_size_request (popover, 340, -1);
  gtk_popover_set_child (GTK_POPOVER (popover), pop_box);

  A.docs_button = GTK_MENU_BUTTON (gtk_menu_button_new ());
  gtk_menu_button_set_icon_name (A.docs_button, "npu-attach-symbolic");
  gtk_menu_button_set_popover (A.docs_button, popover);
  gtk_widget_add_css_class (GTK_WIDGET (A.docs_button), "flat");
  gtk_widget_add_css_class (GTK_WIDGET (A.docs_button), "circular");
  gtk_widget_add_css_class (GTK_WIDGET (A.docs_button), "docs-button");
  gtk_widget_set_valign (GTK_WIDGET (A.docs_button), GTK_ALIGN_END);
  refresh_docs_ui ();
  return GTK_WIDGET (A.docs_button);
}

/* ---- library manager ---- */

static void
on_new_library_apply (AdwEntryRow *row, gpointer user_data)
{
  (void) user_data;
  g_autofree char *name = g_strstrip (g_strdup (gtk_editable_get_text (GTK_EDITABLE (row))));
  if (!*name)
    return;
  g_ptr_array_add (A.libraries, rag_library_new (name, FALSE));
  gtk_editable_set_text (GTK_EDITABLE (row), "");
  refresh_all_docs_ui ();
}

static void
on_lib_add_files (GtkButton *button, gpointer user_data)
{
  (void) user_data;
  RagLibrary *lib = rag_library_find (A.libraries, g_object_get_data (G_OBJECT (button), "id"));
  if (lib)
    choose_files (lib);
}

static void
on_doc_remove (GtkButton *button, gpointer user_data)
{
  (void) user_data;
  RagLibrary *lib = rag_library_find (A.libraries, g_object_get_data (G_OBJECT (button), "lib"));
  if (!lib || lib->busy)
    return;
  rag_library_remove_doc (lib, GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (button), "doc")));
  drop_rag_index ();
  refresh_all_docs_ui ();
}

static void
forget_library (RagLibrary *lib)
{
  for (guint i = 0; i < A.assistants->len; i++)
    {
      Assistant *as = A.assistants->pdata[i];
      for (guint k = 0; as->libs && k < as->libs->len; k++)
        if (g_str_equal (as->libs->pdata[k], lib->id))
          g_ptr_array_remove_index (as->libs, k--);
    }
  assistants_save (A.assistants);
  if (A.conv && conversation_has_lib (A.conv, lib->id))
    {
      conversation_toggle_lib (A.conv, lib->id);
      if (A.conv_saved)
        store_save (A.conv);
    }
  rag_library_delete (lib);
  g_ptr_array_remove (A.libraries, lib);
  drop_rag_index ();
}

static void
on_delete_lib_response (AdwAlertDialog *dialog, const char *response, gpointer user_data)
{
  (void) dialog;
  RagLibrary *lib = rag_library_find (A.libraries, user_data);
  if (g_str_equal (response, "delete") && lib && !lib->busy)
    {
      forget_library (lib);
      refresh_all_docs_ui ();
    }
}

static void
on_lib_delete (GtkButton *button, gpointer user_data)
{
  (void) user_data;
  const char *id = g_object_get_data (G_OBJECT (button), "id");
  RagLibrary *lib = rag_library_find (A.libraries, id);
  if (!lib)
    return;
  AdwDialog *d = TRACK (adw_alert_dialog_new (NULL, NULL));
  adw_alert_dialog_format_heading (ADW_ALERT_DIALOG (d), TR ("¿Eliminar «%s»?", "Delete “%s”?"), lib->name);
  adw_alert_dialog_set_body (ADW_ALERT_DIALOG (d), TR ("Se borra el índice de NPU Chat. Tus archivos originales no se tocan.",
                                                       "NPU Chat's index is deleted. Your original files are not touched."));
  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (d), "cancel", TR ("Cancelar", "Cancel"),
                                  "delete", TR ("Eliminar", "Delete"), NULL);
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (d), "delete", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (d), "cancel");
  g_signal_connect_data (d, "response", G_CALLBACK (on_delete_lib_response), g_strdup (lib->id),
                         free_closure_string, 0);
  adw_dialog_present (d, GTK_WIDGET (A.libs_dialog ? GTK_WIDGET (A.libs_dialog) : GTK_WIDGET (A.win)));
}

static void
on_libs_dialog_closed (AdwDialog *dialog, gpointer user_data)
{
  (void) dialog;
  (void) user_data;
  A.libs_dialog = NULL;
}

static GtkWidget *
build_libraries_page (void)
{
  GtkWidget *page = adw_preferences_page_new ();

  AdwPreferencesGroup *intro = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_description (intro, TR ("Agrupa tus archivos en bibliotecas y actívalas en un chat o en un asistente. "
                                                    "Todo se queda en tu equipo.",
                                                    "Group your files into libraries and turn them on in a chat or an assistant. "
                                                    "Everything stays on your computer."));
  GtkWidget *entry = adw_entry_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (entry), TR ("Nueva biblioteca", "New library"));
  adw_entry_row_set_show_apply_button (ADW_ENTRY_ROW (entry), TRUE);
  g_signal_connect (entry, "apply", G_CALLBACK (on_new_library_apply), NULL);
  adw_preferences_group_add (intro, entry);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), intro);

  for (int pass = 0; pass < 2; pass++)
    {
      gboolean chat_files = pass == 1;
      AdwPreferencesGroup *group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
      guint shown = 0;
      for (guint i = 0; i < A.libraries->len; i++)
        {
          RagLibrary *lib = A.libraries->pdata[i];
          if (lib->chat_scoped != chat_files)
            continue;
          shown++;

          GtkWidget *exp = adw_expander_row_new ();
          adw_preferences_row_set_title (ADW_PREFERENCES_ROW (exp), lib->name);
          adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (exp), FALSE);
          g_autofree char *sub = library_subtitle (lib);
          adw_expander_row_set_subtitle (ADW_EXPANDER_ROW (exp), sub);

          GtkWidget *add = icon_button ("list-add-symbolic", TR ("Añadir archivos", "Add files"));
          g_object_set_data_full (G_OBJECT (add), "id", g_strdup (lib->id), g_free);
          gtk_widget_set_sensitive (add, !lib->busy);
          g_signal_connect (add, "clicked", G_CALLBACK (on_lib_add_files), NULL);
          GtkWidget *del = icon_button ("user-trash-symbolic", TR ("Eliminar biblioteca", "Delete library"));
          g_object_set_data_full (G_OBJECT (del), "id", g_strdup (lib->id), g_free);
          gtk_widget_set_sensitive (del, !lib->busy);
          g_signal_connect (del, "clicked", G_CALLBACK (on_lib_delete), NULL);
          adw_expander_row_add_suffix (ADW_EXPANDER_ROW (exp), add);
          adw_expander_row_add_suffix (ADW_EXPANDER_ROW (exp), del);

          for (guint d = 0; d < lib->docs->len; d++)
            {
              RagDoc *doc = lib->docs->pdata[d];
              GtkWidget *row = adw_action_row_new ();
              adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), doc->name);
              adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
              g_autofree char *info = doc->chunks == 1
                ? g_strdup_printf (TR ("1 fragmento · %s", "1 passage · %s"), doc->path)
                : g_strdup_printf (TR ("%u fragmentos · %s", "%u passages · %s"), doc->chunks, doc->path);
              adw_action_row_set_subtitle (ADW_ACTION_ROW (row), info);
              adw_action_row_set_subtitle_lines (ADW_ACTION_ROW (row), 1);
              GtkWidget *rm = icon_button ("user-trash-symbolic", TR ("Quitar archivo", "Remove file"));
              g_object_set_data_full (G_OBJECT (rm), "lib", g_strdup (lib->id), g_free);
              g_object_set_data (G_OBJECT (rm), "doc", GUINT_TO_POINTER (doc->id));
              gtk_widget_set_sensitive (rm, !lib->busy);
              g_signal_connect (rm, "clicked", G_CALLBACK (on_doc_remove), NULL);
              adw_action_row_add_suffix (ADW_ACTION_ROW (row), rm);
              adw_expander_row_add_row (ADW_EXPANDER_ROW (exp), row);
            }
          adw_preferences_group_add (group, exp);
        }
      if (shown)
        {
          adw_preferences_group_set_title (group, chat_files ? TR ("Archivos de chats", "Chat files")
                                                             : TR ("Bibliotecas", "Libraries"));
          adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), group);
        }
      else
        g_object_unref (g_object_ref_sink (group));
    }
  return page;
}

static void
libs_dialog_refresh (void)
{
  if (!A.libs_dialog)
    return;
  GtkWidget *tv = adw_dialog_get_child (A.libs_dialog);
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (tv), build_libraries_page ());
}

static void
open_libraries_dialog (void)
{
  if (A.libs_dialog)
    return;
  A.libs_dialog = TRACK (adw_dialog_new ());
  adw_dialog_set_title (A.libs_dialog, TR ("Bibliotecas de documentos", "Document libraries"));
  adw_dialog_set_content_width (A.libs_dialog, 600);
  adw_dialog_set_content_height (A.libs_dialog, 680);
  GtkWidget *tv = adw_toolbar_view_new ();
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (tv), adw_header_bar_new ());
  adw_dialog_set_child (A.libs_dialog, tv);
  g_signal_connect (A.libs_dialog, "closed", G_CALLBACK (on_libs_dialog_closed), NULL);
  libs_dialog_refresh ();
  adw_dialog_present (A.libs_dialog, GTK_WIDGET (A.win));
}

static void
act_libraries (GSimpleAction *a, GVariant *p, gpointer d)
{
  (void) a; (void) p; (void) d;
  open_libraries_dialog ();
}

/* ---- UI construction -------------------------------------------------- */

static GtkWidget *
build_sidebar (GMenuModel *menu)
{
  GtkWidget *tv = adw_toolbar_view_new ();
  GtkWidget *header = adw_header_bar_new ();
  adw_header_bar_set_title_widget (ADW_HEADER_BAR (header), adw_window_title_new ("NPU Chat", NULL));

  GtkWidget *new_btn = gtk_button_new_from_icon_name ("npu-new-chat-symbolic");
  gtk_widget_set_tooltip_text (new_btn, TR ("Nueva conversación (Ctrl+N)", "New chat (Ctrl+N)"));
  gtk_actionable_set_action_name (GTK_ACTIONABLE (new_btn), "win.new-chat");
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), new_btn);

  GtkWidget *menu_btn = gtk_menu_button_new ();
  gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (menu_btn), "open-menu-symbolic");
  gtk_menu_button_set_menu_model (GTK_MENU_BUTTON (menu_btn), menu);
  gtk_menu_button_set_primary (GTK_MENU_BUTTON (menu_btn), TRUE);
  gtk_widget_set_tooltip_text (menu_btn, TR ("Menú principal", "Main menu"));
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), menu_btn);
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (tv), header);

  A.chat_list = GTK_LIST_BOX (gtk_list_box_new ());
  gtk_widget_add_css_class (GTK_WIDGET (A.chat_list), "navigation-sidebar");
  gtk_list_box_set_header_func (A.chat_list, chat_header_func, NULL, NULL);
  g_signal_connect (A.chat_list, "row-activated", G_CALLBACK (on_chat_activated), NULL);
  GtkWidget *sw = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (sw), GTK_WIDGET (A.chat_list));

  GtkWidget *empty = adw_status_page_new ();
  adw_status_page_set_icon_name (ADW_STATUS_PAGE (empty), APP_ID);
  adw_status_page_set_title (ADW_STATUS_PAGE (empty), TR ("Sin conversaciones", "No chats yet"));
  adw_status_page_set_description (ADW_STATUS_PAGE (empty), TR ("Tus chats aparecerán aquí.", "Your chats will show up here."));
  gtk_widget_add_css_class (empty, "compact");

  A.chat_list_stack = GTK_STACK (gtk_stack_new ());
  gtk_stack_add_named (A.chat_list_stack, sw, "list");
  gtk_stack_add_named (A.chat_list_stack, empty, "empty");
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (tv), GTK_WIDGET (A.chat_list_stack));

  GtkWidget *power = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class (power, "power-bar");
  A.power_icon = GTK_IMAGE (gtk_image_new ());
  A.power_label = GTK_LABEL (gtk_label_new (NULL));
  gtk_label_set_xalign (A.power_label, 0);
  gtk_label_set_ellipsize (A.power_label, PANGO_ELLIPSIZE_END);
  gtk_box_append (GTK_BOX (power), GTK_WIDGET (A.power_icon));
  gtk_box_append (GTK_BOX (power), GTK_WIDGET (A.power_label));
  adw_toolbar_view_add_bottom_bar (ADW_TOOLBAR_VIEW (tv), power);
  return tv;
}

static GtkWidget *
build_model_button (void)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *text = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  A.model_label = GTK_LABEL (gtk_label_new (NULL));
  gtk_label_set_ellipsize (A.model_label, PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class (GTK_WIDGET (A.model_label), "heading");

  GtkWidget *state_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_halign (state_box, GTK_ALIGN_CENTER);
  A.state_dot = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (A.state_dot, "status-dot");
  gtk_widget_set_valign (A.state_dot, GTK_ALIGN_CENTER);
  A.state_label = GTK_LABEL (gtk_label_new (NULL));
  gtk_label_set_ellipsize (A.state_label, PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class (GTK_WIDGET (A.state_label), "caption");
  gtk_widget_add_css_class (GTK_WIDGET (A.state_label), "dim-label");
  gtk_box_append (GTK_BOX (state_box), A.state_dot);
  gtk_box_append (GTK_BOX (state_box), GTK_WIDGET (A.state_label));

  gtk_box_append (GTK_BOX (text), GTK_WIDGET (A.model_label));
  gtk_box_append (GTK_BOX (text), state_box);
  gtk_box_append (GTK_BOX (box), text);
  gtk_box_append (GTK_BOX (box), gtk_image_new_from_icon_name ("pan-down-symbolic"));

  /* Popover: installed chat models + manage button. */
  GtkWidget *pop_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_add_css_class (pop_box, "picker");
  GtkWidget *title = dim_label (TR ("Modelos instalados", "Installed models"), "caption-heading");
  gtk_widget_set_margin_start (title, 8);
  A.picker = GTK_LIST_BOX (gtk_list_box_new ());
  gtk_list_box_set_selection_mode (A.picker, GTK_SELECTION_NONE);
  gtk_widget_add_css_class (GTK_WIDGET (A.picker), "boxed-list");
  GtkWidget *ph = dim_label (TR ("Sin modelos todavía", "No models yet"), "placeholder-row");
  gtk_label_set_xalign (GTK_LABEL (ph), 0.5);
  gtk_list_box_set_placeholder (A.picker, ph);
  g_signal_connect (A.picker, "row-activated", G_CALLBACK (on_picker_activated), NULL);
  GtkWidget *pick_sw = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (pick_sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (pick_sw), TRUE);
  gtk_scrolled_window_set_max_content_height (GTK_SCROLLED_WINDOW (pick_sw), 360);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (pick_sw), GTK_WIDGET (A.picker));

  GtkWidget *manage = gtk_button_new_with_label (TR ("Administrar modelos…", "Manage models…"));
  gtk_widget_add_css_class (manage, "flat");
  gtk_actionable_set_action_name (GTK_ACTIONABLE (manage), "win.models");

  gtk_box_append (GTK_BOX (pop_box), title);
  gtk_box_append (GTK_BOX (pop_box), pick_sw);
  gtk_box_append (GTK_BOX (pop_box), manage);

  GtkWidget *popover = gtk_popover_new ();
  gtk_widget_set_size_request (popover, 340, -1);
  gtk_popover_set_child (GTK_POPOVER (popover), pop_box);

  A.model_button = GTK_MENU_BUTTON (gtk_menu_button_new ());
  gtk_menu_button_set_child (A.model_button, box);
  gtk_menu_button_set_popover (A.model_button, popover);
  gtk_widget_add_css_class (GTK_WIDGET (A.model_button), "flat");
  gtk_widget_add_css_class (GTK_WIDGET (A.model_button), "model-button");
  return GTK_WIDGET (A.model_button);
}

static GtkWidget *
build_welcome (void)
{
  GtkWidget *page = adw_status_page_new ();
  A.welcome = ADW_STATUS_PAGE (page);
  adw_status_page_set_icon_name (A.welcome, APP_ID);

  GtkWidget *flow = gtk_flow_box_new ();
  gtk_flow_box_set_selection_mode (GTK_FLOW_BOX (flow), GTK_SELECTION_NONE);
  gtk_flow_box_set_max_children_per_line (GTK_FLOW_BOX (flow), 2);
  gtk_flow_box_set_homogeneous (GTK_FLOW_BOX (flow), TRUE);
  gtk_flow_box_set_column_spacing (GTK_FLOW_BOX (flow), 10);
  gtk_flow_box_set_row_spacing (GTK_FLOW_BOX (flow), 10);
  gtk_widget_set_halign (flow, GTK_ALIGN_CENTER);

  const char *suggestions[] = {
    TR ("Explícame qué es una NPU en palabras simples", "Explain what an NPU is in simple words"),
    TR ("Escribe un poema corto sobre el café", "Write a short poem about coffee"),
    TR ("Dame 5 ideas para una cena rápida", "Give me 5 quick dinner ideas"),
    TR ("Escribe una función en C que invierta un string", "Write a C function that reverses a string"),
  };
  for (guint i = 0; i < G_N_ELEMENTS (suggestions); i++)
    {
      GtkWidget *b = gtk_button_new_with_label (suggestions[i]);
      gtk_widget_add_css_class (b, "suggestion");
      GtkWidget *label = gtk_button_get_child (GTK_BUTTON (b));
      gtk_label_set_wrap (GTK_LABEL (label), TRUE);
      gtk_label_set_max_width_chars (GTK_LABEL (label), 26);
      gtk_label_set_xalign (GTK_LABEL (label), 0);
      g_signal_connect (b, "clicked", G_CALLBACK (on_suggestion), NULL);
      gtk_flow_box_append (GTK_FLOW_BOX (flow), b);
    }
  A.suggestions = flow;
  adw_status_page_set_child (A.welcome, flow);
  return page;
}

static GtkWidget *
pill_button (const char *label, const char *action, gboolean suggested)
{
  GtkWidget *b = gtk_button_new_with_label (label);
  gtk_widget_add_css_class (b, "pill");
  if (suggested)
    gtk_widget_add_css_class (b, "suggested-action");
  if (action)
    gtk_actionable_set_action_name (GTK_ACTIONABLE (b), action);
  return b;
}

static GtkWidget *
build_setup_page (void)
{
  static const char commands[] =
    "sudo dnf copr enable alessandrolattao/fastflowlm\n"
    "sudo dnf install --exclude=xdna-driver-dkms fastflowlm\n"
    "sudo flm-fetch-kernels";

  GtkWidget *page = adw_status_page_new ();
  adw_status_page_set_icon_name (ADW_STATUS_PAGE (page), APP_ID);
  adw_status_page_set_title (ADW_STATUS_PAGE (page), TR ("Configura la NPU", "Set up the NPU"));
  adw_status_page_set_description (ADW_STATUS_PAGE (page),
                                   TR ("NPU Chat usa FastFlowLM para correr modelos en la NPU. "
                                       "Instálalo una sola vez pegando esto en Konsole:",
                                       "NPU Chat uses FastFlowLM to run models on the NPU. "
                                       "Install it once by pasting this into Konsole:"));

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 18);
  gtk_widget_set_halign (box, GTK_ALIGN_CENTER);

  GtkWidget *cmd = gtk_label_new (commands);
  gtk_label_set_selectable (GTK_LABEL (cmd), TRUE);
  gtk_label_set_xalign (GTK_LABEL (cmd), 0);
  gtk_widget_add_css_class (cmd, "cmd-card");

  GtkWidget *buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_halign (buttons, GTK_ALIGN_CENTER);
  GtkWidget *copy = pill_button (TR ("Copiar comandos", "Copy commands"), NULL, FALSE);
  g_object_set_data (G_OBJECT (copy), "text", (gpointer) commands);
  g_signal_connect (copy, "clicked", G_CALLBACK (on_copy_commands), NULL);
  gtk_box_append (GTK_BOX (buttons), copy);
  gtk_box_append (GTK_BOX (buttons), pill_button (TR ("Ya lo instalé", "I installed it"), "win.refresh", TRUE));

  GtkWidget *hint = dim_label (TR ("Si falta algo más, NPU Chat te lo indicará con un aviso.",
                                   "If anything else is missing, NPU Chat will tell you with a notice."), "caption");
  gtk_label_set_xalign (GTK_LABEL (hint), 0.5);
  gtk_label_set_justify (GTK_LABEL (hint), GTK_JUSTIFY_CENTER);

  gtk_box_append (GTK_BOX (box), cmd);
  gtk_box_append (GTK_BOX (box), buttons);
  gtk_box_append (GTK_BOX (box), hint);
  adw_status_page_set_child (ADW_STATUS_PAGE (page), box);
  return page;
}

static GtkWidget *
build_nomodels_page (void)
{
  GtkWidget *page = adw_status_page_new ();
  adw_status_page_set_icon_name (ADW_STATUS_PAGE (page), "folder-download-symbolic");
  adw_status_page_set_title (ADW_STATUS_PAGE (page), TR ("Descarga tu primer modelo", "Download your first model"));
  adw_status_page_set_description (ADW_STATUS_PAGE (page),
                                   TR ("Te mostramos solo los que funcionan bien en tu equipo.",
                                       "We only show the ones that run well on your computer."));
  GtkWidget *b = pill_button (TR ("Ver modelos compatibles", "See compatible models"), "win.download", TRUE);
  gtk_widget_set_halign (b, GTK_ALIGN_CENTER);
  adw_status_page_set_child (ADW_STATUS_PAGE (page), b);
  return page;
}

static GtkWidget *
build_composer (void)
{
  GtkWidget *composer = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class (composer, "composer");

  A.input = GTK_TEXT_VIEW (gtk_text_view_new ());
  gtk_text_view_set_wrap_mode (A.input, GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_accepts_tab (A.input, FALSE);
  gtk_widget_set_hexpand (GTK_WIDGET (A.input), TRUE);
  gtk_widget_set_valign (GTK_WIDGET (A.input), GTK_ALIGN_CENTER);

  GtkEventController *keys = gtk_event_controller_key_new ();
  gtk_event_controller_set_propagation_phase (keys, GTK_PHASE_CAPTURE);
  g_signal_connect (keys, "key-pressed", G_CALLBACK (on_input_key), NULL);
  gtk_widget_add_controller (GTK_WIDGET (A.input), keys);
  g_signal_connect (gtk_text_view_get_buffer (A.input), "changed", G_CALLBACK (on_buffer_changed), NULL);

  /* EXTERNAL: no scrollbar, so the box is one line tall until the text
   * grows (a visible scrollbar would force a two-line minimum height). */
  GtkWidget *sw = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (sw), GTK_POLICY_NEVER, GTK_POLICY_EXTERNAL);
  gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (sw), TRUE);
  gtk_scrolled_window_set_max_content_height (GTK_SCROLLED_WINDOW (sw), 180);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (sw), GTK_WIDGET (A.input));
  gtk_widget_set_hexpand (sw, TRUE);
  gtk_widget_set_valign (sw, GTK_ALIGN_CENTER);

  A.placeholder = gtk_label_new (TR ("Escribe un mensaje…", "Type a message…"));
  gtk_label_set_xalign (GTK_LABEL (A.placeholder), 0);
  gtk_widget_set_halign (A.placeholder, GTK_ALIGN_START);
  gtk_widget_set_valign (A.placeholder, GTK_ALIGN_CENTER);
  gtk_widget_set_can_target (A.placeholder, FALSE);
  gtk_widget_add_css_class (A.placeholder, "placeholder");

  GtkWidget *overlay = gtk_overlay_new ();
  gtk_overlay_set_child (GTK_OVERLAY (overlay), sw);
  gtk_overlay_add_overlay (GTK_OVERLAY (overlay), A.placeholder);
  gtk_widget_set_hexpand (overlay, TRUE);

  A.send = GTK_BUTTON (gtk_button_new_from_icon_name ("go-up-symbolic"));
  gtk_widget_set_valign (GTK_WIDGET (A.send), GTK_ALIGN_END);
  gtk_widget_add_css_class (GTK_WIDGET (A.send), "circular");
  gtk_widget_add_css_class (GTK_WIDGET (A.send), "suggested-action");
  gtk_widget_add_css_class (GTK_WIDGET (A.send), "send");
  gtk_widget_set_valign (GTK_WIDGET (A.send), GTK_ALIGN_END);
  g_signal_connect (A.send, "clicked", G_CALLBACK (on_send_clicked), NULL);

  gtk_box_append (GTK_BOX (composer), build_docs_button ());
  gtk_box_append (GTK_BOX (composer), overlay);
  gtk_box_append (GTK_BOX (composer), GTK_WIDGET (A.send));

  /* Shows the progress of a file import just above the input. */
  A.import_label = GTK_LABEL (gtk_label_new (NULL));
  gtk_label_set_xalign (A.import_label, 0);
  gtk_label_set_ellipsize (A.import_label, PANGO_ELLIPSIZE_MIDDLE);
  gtk_widget_add_css_class (GTK_WIDGET (A.import_label), "caption");
  A.import_bar = gtk_progress_bar_new ();
  GtkWidget *import_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_add_css_class (import_box, "import-bar");
  gtk_box_append (GTK_BOX (import_box), GTK_WIDGET (A.import_label));
  gtk_box_append (GTK_BOX (import_box), A.import_bar);
  A.import_revealer = gtk_revealer_new ();
  gtk_revealer_set_transition_type (GTK_REVEALER (A.import_revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP);
  gtk_revealer_set_child (GTK_REVEALER (A.import_revealer), import_box);

  GtkWidget *column = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append (GTK_BOX (column), A.import_revealer);
  gtk_box_append (GTK_BOX (column), composer);

  GtkWidget *clamp = adw_clamp_new ();
  adw_clamp_set_maximum_size (ADW_CLAMP (clamp), 820);
  adw_clamp_set_child (ADW_CLAMP (clamp), column);
  gtk_widget_add_css_class (clamp, "composer-area");
  return clamp;
}

static GtkWidget *
build_content (void)
{
  GtkWidget *tv = adw_toolbar_view_new ();
  adw_toolbar_view_set_top_bar_style (ADW_TOOLBAR_VIEW (tv), ADW_TOOLBAR_FLAT);

  GtkWidget *header = adw_header_bar_new ();
  GtkWidget *selectors = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 2);
  gtk_box_append (GTK_BOX (selectors), build_assistant_button ());
  GtkWidget *sep = gtk_separator_new (GTK_ORIENTATION_VERTICAL);
  gtk_widget_set_margin_top (sep, 10);
  gtk_widget_set_margin_bottom (sep, 10);
  gtk_box_append (GTK_BOX (selectors), sep);
  gtk_box_append (GTK_BOX (selectors), build_model_button ());
  adw_header_bar_set_title_widget (ADW_HEADER_BAR (header), selectors);
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (tv), header);

  A.banner = ADW_BANNER (adw_banner_new (TR ("La NPU necesita un ajuste del sistema para funcionar",
                                             "The NPU needs a system setting to work")));
  adw_banner_set_button_label (A.banner, TR ("Cómo arreglarlo", "How to fix"));
  g_signal_connect_swapped (A.banner, "button-clicked", G_CALLBACK (show_memlock_dialog), NULL);
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (tv), GTK_WIDGET (A.banner));

  A.messages = gtk_box_new (GTK_ORIENTATION_VERTICAL, 22);
  gtk_widget_add_css_class (A.messages, "messages");
  GtkWidget *clamp = adw_clamp_new ();
  adw_clamp_set_maximum_size (ADW_CLAMP (clamp), 820);
  adw_clamp_set_child (ADW_CLAMP (clamp), A.messages);

  A.scroller = GTK_SCROLLED_WINDOW (gtk_scrolled_window_new ());
  gtk_scrolled_window_set_policy (A.scroller, GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (A.scroller, clamp);
  GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment (A.scroller);
  g_signal_connect (adj, "value-changed", G_CALLBACK (on_adj_value), NULL);
  g_signal_connect (adj, "changed", G_CALLBACK (on_adj_changed), NULL);

  A.chat_stack = GTK_STACK (gtk_stack_new ());
  gtk_stack_set_transition_type (A.chat_stack, GTK_STACK_TRANSITION_TYPE_CROSSFADE);
  gtk_stack_add_named (A.chat_stack, build_welcome (), "welcome");
  gtk_stack_add_named (A.chat_stack, build_setup_page (), "setup");
  gtk_stack_add_named (A.chat_stack, build_nomodels_page (), "nomodels");
  gtk_stack_add_named (A.chat_stack, GTK_WIDGET (A.scroller), "chat");
  gtk_widget_set_vexpand (GTK_WIDGET (A.chat_stack), TRUE);

  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (tv), GTK_WIDGET (A.chat_stack));
  adw_toolbar_view_add_bottom_bar (ADW_TOOLBAR_VIEW (tv), build_composer ());
  return tv;
}

static GMenuModel *
build_menu (void)
{
  GMenu *menu = g_menu_new ();
  GMenu *top = g_menu_new ();
  g_menu_append (top, TR ("Nueva conversación", "New chat"), "win.new-chat");
  g_menu_append (top, TR ("Galería de asistentes", "Assistant gallery"), "win.gallery");
  g_menu_append (top, TR ("Bibliotecas de documentos", "Document libraries"), "win.libraries");
  g_menu_append (top, TR ("Calendario", "Calendar"), "win.calendar");
  g_menu_append (top, TR ("Habilidades", "Skills"), "win.skills");
  g_menu_append (top, TR ("Administrar modelos", "Manage models"), "win.models");
  g_menu_append_section (menu, NULL, G_MENU_MODEL (top));
  GMenu *bottom = g_menu_new ();
  g_menu_append (bottom, TR ("Preferencias", "Preferences"), "win.preferences");
  g_menu_append (bottom, TR ("Acerca de NPU Chat", "About NPU Chat"), "win.about");
  g_menu_append (bottom, TR ("Salir", "Quit"), "win.quit");
  g_menu_append_section (menu, NULL, G_MENU_MODEL (bottom));
  g_object_unref (top);
  g_object_unref (bottom);
  return G_MENU_MODEL (menu);
}

/* (Re)creates every widget; called at startup and when the language changes. */
static void
build_ui (void)
{
  g_autoptr (GMenuModel) menu = build_menu ();

  A.split = ADW_NAVIGATION_SPLIT_VIEW (adw_navigation_split_view_new ());
  adw_navigation_split_view_set_min_sidebar_width (A.split, 250);
  adw_navigation_split_view_set_max_sidebar_width (A.split, 310);
  adw_navigation_split_view_set_sidebar (A.split, adw_navigation_page_new (build_sidebar (menu), "NPU Chat"));
  adw_navigation_split_view_set_content (A.split, adw_navigation_page_new (build_content (), TR ("Chat", "Chat")));

  A.toasts = ADW_TOAST_OVERLAY (adw_toast_overlay_new ());
  adw_toast_overlay_set_child (A.toasts, GTK_WIDGET (A.split));

  /* Dropping files anywhere in the window adds them to the open chat. */
  GtkDropTarget *drop = gtk_drop_target_new (GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
  g_signal_connect (drop, "drop", G_CALLBACK (on_files_dropped), NULL);
  gtk_widget_add_controller (GTK_WIDGET (A.toasts), GTK_EVENT_CONTROLLER (drop));

  /* The breakpoint lives in a bin that is rebuilt with the rest of the UI,
   * so it never points at a stale split view. */
  GtkWidget *bin = adw_breakpoint_bin_new ();
  gtk_widget_set_size_request (bin, 360, 360);
  AdwBreakpoint *bp = adw_breakpoint_new (adw_breakpoint_condition_parse ("max-width: 640sp"));
  GValue v = G_VALUE_INIT;
  g_value_init (&v, G_TYPE_BOOLEAN);
  g_value_set_boolean (&v, TRUE);
  adw_breakpoint_add_setter (bp, G_OBJECT (A.split), "collapsed", &v);
  g_value_unset (&v);
  adw_breakpoint_bin_add_breakpoint (ADW_BREAKPOINT_BIN (bin), bp);
  adw_breakpoint_bin_set_child (ADW_BREAKPOINT_BIN (bin), GTK_WIDGET (A.toasts));
  adw_application_window_set_content (ADW_APPLICATION_WINDOW (A.win), bin);

  update_power_bar ();
}

/* ---- lifecycle -------------------------------------------------------- */

static gboolean
on_close_request (GtkWindow *win, gpointer user_data)
{
  (void) user_data;
  A.width = gtk_widget_get_width (GTK_WIDGET (win));
  A.height = gtk_widget_get_height (GTK_WIDGET (win));
  settings_save ();

  A.quitting = TRUE;
  disarm_idle ();
  g_clear_handle_id (&A.render_id, g_source_remove);
  if (A.reply)
    {
      /* Let the cancelled request unwind so nothing is left allocated. */
      g_cancellable_cancel (A.cancel);
      gint64 deadline = g_get_monotonic_time () + G_USEC_PER_SEC;
      while (A.reply && g_get_monotonic_time () < deadline)
        g_main_context_iteration (NULL, FALSE);
      if (A.reply)
        finish_reply (NULL);
    }
  return FALSE;
}

#ifdef NPU_CHAT_SELFTEST
/* Objects passed to TRACK() that are still alive, keyed by object. */
static GHashTable *tracked;

static void
selftest_gone (gpointer data, GObject *where)
{
  (void) data;
  g_hash_table_remove (tracked, where);
}

gpointer
selftest_track (gpointer object, const char *what)
{
  if (!tracked)
    tracked = g_hash_table_new (NULL, NULL);
  g_hash_table_insert (tracked, object, (gpointer) what);
  g_object_weak_ref (object, selftest_gone, NULL);
  return object;
}

static void
selftest_report_live_objects (void)
{
  GHashTableIter it;
  gpointer obj, what;
  if (tracked)
    {
      g_hash_table_iter_init (&it, tracked);
      while (g_hash_table_iter_next (&it, &obj, &what))
        g_printerr ("SELFTEST-LIVE %s created at %s (refcount %u)\n",
                    G_OBJECT_TYPE_NAME (obj), (const char *) what, G_OBJECT (obj)->ref_count);
    }
  g_printerr ("SELFTEST-LIVE-DONE %u tracked objects alive\n", tracked ? g_hash_table_size (tracked) : 0);
}
#endif

static void
on_shutdown (GApplication *app, gpointer user_data)
{
  (void) app;
  (void) user_data;
  flm_shutdown ();
  calendar_subscriptions_stop ();
  calendar_alerts_stop ();
  calendar_default_free ();
  rag_shutdown ();
  power_shutdown ();
  updater_shutdown ();
  update_info_free (g_steal_pointer (&A.update));
  if (A.restart_after_exit)
    updater_restart_after_exit ();

  if (A.conv && !A.conv_saved)
    conversation_free (A.conv);
  A.conv = NULL;
  g_clear_pointer (&A.convs, g_ptr_array_unref);
  g_clear_pointer (&A.assistants, g_ptr_array_unref);
  drop_rag_index ();
  g_clear_pointer (&A.libraries, g_ptr_array_unref);
  g_clear_pointer (&A.calls, g_ptr_array_unref);
  g_clear_pointer (&A.skills_on, g_hash_table_unref);
  g_clear_pointer (&A.pick_id, g_free);
  g_clear_pointer (&A.pick_team, g_ptr_array_unref);
  g_clear_pointer (&A.queue, g_array_unref);
  g_clear_pointer (&A.installed, g_ptr_array_unref);
  g_clear_pointer (&A.pulling, g_hash_table_unref);
  g_clear_pointer (&A.md.sys, sysinfo_free);
  g_clear_object (&A.cancel);
  if (A.http_error)
    g_string_free (A.http_error, TRUE);
  A.http_error = NULL;
  g_clear_pointer (&A.model, g_free);
  g_clear_pointer (&A.pmode, g_free);
  g_clear_pointer (&A.lang, g_free);
  g_clear_pointer (&A.theme, g_free);

#ifdef NPU_CHAT_SELFTEST
  if (g_getenv ("SELFTEST"))
    selftest_report_live_objects ();
#endif
}

static void
on_activate (GApplication *app, gpointer user_data)
{
  (void) user_data;

  if (A.win)
    {
      gtk_window_present (A.win);
      if (A.input)
        gtk_widget_grab_focus (GTK_WIDGET (A.input));
      return;
    }

  settings_load ();
  calendar_alerts_start (G_APPLICATION (app));
  calendar_subscriptions_start (calendar_ui_refresh);
  i18n_set (A.lang);
  apply_theme ();

  GtkCssProvider *css = gtk_css_provider_new ();
  gtk_css_provider_load_from_resource (css, "/io/github/vezzulab/NpuChat/style.css");
  gtk_style_context_add_provider_for_display (gdk_display_get_default (), GTK_STYLE_PROVIDER (css),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref (css);
  gtk_icon_theme_add_resource_path (gtk_icon_theme_get_for_display (gdk_display_get_default ()),
                                    "/io/github/vezzulab/NpuChat/icons");

  A.http_error = g_string_new (NULL);
  A.pulling = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  A.convs = store_load_all ();
  A.assistants = assistants_load ();
  A.libraries = rag_libraries_load ();
  A.calls = g_ptr_array_new_with_free_func (pending_call_free);
  A.pick_team = g_ptr_array_new_with_free_func (g_free);
  A.queue = g_array_new (FALSE, FALSE, sizeof (int));
  A.stick_bottom = TRUE;

  A.win = GTK_WINDOW (adw_application_window_new (GTK_APPLICATION (app)));
  gtk_window_set_title (A.win, "NPU Chat");
  gtk_window_set_icon_name (A.win, APP_ID);
  gtk_window_set_default_size (A.win, A.width, A.height);
  g_signal_connect (A.win, "close-request", G_CALLBACK (on_close_request), NULL);

  const GActionEntry entries[] = {
    { "new-chat", act_new_chat, NULL, NULL, NULL, { 0 } },
    { "models", act_models, NULL, NULL, NULL, { 0 } },
    { "download", act_download, NULL, NULL, NULL, { 0 } },
    { "preferences", act_prefs, NULL, NULL, NULL, { 0 } },
    { "refresh", act_refresh, NULL, NULL, NULL, { 0 } },
    { "retry", act_retry, NULL, NULL, NULL, { 0 } },
    { "about", act_about, NULL, NULL, NULL, { 0 } },
    { "new-assistant", act_new_assistant, NULL, NULL, NULL, { 0 } },
    { "gallery", act_gallery, NULL, NULL, NULL, { 0 } },
    { "libraries", act_libraries, NULL, NULL, NULL, { 0 } },
    { "skills", act_skills, NULL, NULL, NULL, { 0 } },
    { "calendar", act_calendar, NULL, NULL, NULL, { 0 } },
    { "install-update", act_install_update, NULL, NULL, NULL, { 0 } },
    { "open-release", act_open_release, NULL, NULL, NULL, { 0 } },
    { "quit", act_quit, NULL, NULL, NULL, { 0 } },
  };
  g_action_map_add_action_entries (G_ACTION_MAP (A.win), entries, G_N_ELEMENTS (entries), NULL);

  static const struct {
    const char *action;
    const char *accel;
  } accels[] = {
    { "win.new-chat", "<Control>n" },
    { "win.models", "<Control>m" },
    { "win.preferences", "<Control>comma" },
    { "win.quit", "<Control>q" },
  };
  for (guint i = 0; i < G_N_ELEMENTS (accels); i++)
    {
      const char *list[] = { accels[i].accel, NULL };
      gtk_application_set_accels_for_action (GTK_APPLICATION (app), accels[i].action, list);
    }

  power_init (on_power_changed, NULL);
  flm_init (on_flm_state, NULL);

  build_ui ();
  A.conv = conversation_new ();
  A.conv_saved = FALSE;
  refresh_chat_list ();
  render_conversation ();
  refresh_models ();
  if (A.flm_present)
    flm_detect (on_detected, NULL);

  gtk_window_present (A.win);
  gtk_widget_grab_focus (GTK_WIDGET (A.input));

  /* A little after startup, so it never slows the window down. */
  g_timeout_add_seconds (8, auto_update_check, NULL);

#ifdef NPU_CHAT_SELFTEST
  if (g_getenv ("SELFTEST"))
    g_timeout_add (200, selftest_tick, NULL);
#endif
}

#ifdef NPU_CHAT_SELFTEST
/* Test-only automation (build-test/ only): SELFTEST="send:hi;wait;models;sleep:2;close". */
static gboolean
selftest_tick (gpointer user_data)
{
  (void) user_data;
  static char **steps;
  static guint i;
  static gint64 until;

  if (!steps)
    steps = g_strsplit (g_getenv ("SELFTEST"), ";", -1);
  if (until && g_get_monotonic_time () < until)
    return G_SOURCE_CONTINUE;
  until = 0;
  if (!steps[i])
    {
      g_clear_pointer (&steps, g_strfreev);
      return G_SOURCE_REMOVE;
    }

  const char *s = steps[i];
  if (g_str_has_prefix (s, "send:"))
    {
      if (A.reply || !A.installed)
        return G_SOURCE_CONTINUE;
      send_text (s + 5);
    }
  else if (g_str_equal (s, "wait") && A.reply)
    return G_SOURCE_CONTINUE;
  else if (g_str_has_prefix (s, "sleep:"))
    until = g_get_monotonic_time () + (gint64) (g_ascii_strtod (s + 6, NULL) * G_USEC_PER_SEC);
  else if (g_str_equal (s, "models"))
    open_models_dialog (TRUE);
  else if (g_str_equal (s, "installed"))
    open_models_dialog (FALSE);
  else if (g_str_equal (s, "closedlg"))
    {
      if (A.md.dialog)
        adw_dialog_force_close (A.md.dialog);
      if (A.prefs)
        adw_dialog_force_close (A.prefs);
      if (editor_current)
        adw_dialog_force_close (editor_current->dialog);
      if (gallery.dialog)
        adw_dialog_force_close (gallery.dialog);
      if (A.libs_dialog)
        adw_dialog_force_close (A.libs_dialog);
      if (A.skills_dialog)
        adw_dialog_force_close (A.skills_dialog);
      calendar_ui_close ();
      if (A.update_dialog)
        adw_dialog_force_close (A.update_dialog);
    }
  else if (g_str_equal (s, "prefs"))
    open_preferences ();
  else if (g_str_has_prefix (s, "lang:"))
    {
      g_free (A.lang);
      A.lang = g_strdup (s + 5);
      rebuild_idle (NULL);
    }
  else if (g_str_has_prefix (s, "theme:"))
    {
      g_free (A.theme);
      A.theme = g_strdup (s + 6);
      apply_theme ();
    }
  else if (g_str_equal (s, "newchat"))
    new_chat ();
  else if (g_str_has_prefix (s, "team:"))
    {
      g_auto (GStrv) idx = g_strsplit (s + 5, ",", -1);
      A.team_mode = TRUE;
      g_ptr_array_set_size (A.pick_team, 0);
      for (int k = 0; idx[k]; k++)
        if ((guint) atoi (idx[k]) < A.assistants->len)
          g_ptr_array_add (A.pick_team, g_strdup (((Assistant *) A.assistants->pdata[atoi (idx[k])])->id));
      update_assistant_ui ();
    }
  else if (g_str_has_prefix (s, "toggle:") && (guint) atoi (s + 7) < A.assistants->len)
    toggle_team_member (((Assistant *) A.assistants->pdata[atoi (s + 7)])->id);
  else if (g_str_equal (s, "clearassistant"))
    on_clear_assistant (NULL, NULL);
  else if (g_str_has_prefix (s, "galleryremove:"))
    gallery_remove (s + 14);
  else if (g_str_has_prefix (s, "pick:"))
    {
      guint n = (guint) atoi (s + 5);
      choose_assistant (n < A.assistants->len ? ((Assistant *) A.assistants->pdata[n])->id : NULL);
    }
  else if (g_str_equal (s, "assistmenu"))
    gtk_menu_button_popup (A.assistant_button);
  else if (g_str_equal (s, "popdown"))
    gtk_menu_button_popdown (A.assistant_button);
  else if (g_str_has_prefix (s, "snap:"))
    {
      /* Renders the window itself at 2x, independent of focus or compositor. */
      GtkWidget *w = GTK_WIDGET (A.win);
      int wd = gtk_widget_get_width (w), ht = gtk_widget_get_height (w);
      g_autoptr (GdkPaintable) paintable = gtk_widget_paintable_new (w);
      GtkSnapshot *snap = gtk_snapshot_new ();
      gtk_snapshot_scale (snap, 2, 2);
      gdk_paintable_snapshot (paintable, snap, wd, ht);
      g_autoptr (GskRenderNode) node = gtk_snapshot_free_to_node (snap);
      GskRenderer *renderer = gtk_native_get_renderer (GTK_NATIVE (w));
      g_autoptr (GdkTexture) tex = gsk_renderer_render_texture (renderer, node,
                                                                &GRAPHENE_RECT_INIT (0, 0, wd * 2, ht * 2));
      gdk_texture_save_to_png (tex, s + 5);
    }
  else if (g_str_equal (s, "checkupdate"))
    updater_check (on_update_checked, GINT_TO_POINTER (TRUE));
  else if (g_str_equal (s, "update"))
    act_install_update (NULL, NULL, NULL);
  else if (g_str_has_prefix (s, "import:"))
    {
      g_auto (GStrv) paths = g_strsplit (s + 7, "|", -1);
      import_into_chat (paths);
    }
  else if (g_str_equal (s, "waitimport"))
    {
      for (guint k = 0; A.libraries && k < A.libraries->len; k++)
        if (((RagLibrary *) A.libraries->pdata[k])->busy)
          return G_SOURCE_CONTINUE;
    }
  else if (g_str_has_prefix (s, "newlib:"))
    {
      g_ptr_array_add (A.libraries, rag_library_new (s + 7, FALSE));
      refresh_all_docs_ui ();
    }
  else if (g_str_has_prefix (s, "libimport:") && A.libraries->len)
    {
      /* libimport:<n>:<path>|<path>... where n indexes the libraries */
      char *colon = strchr (s + 10, ':');
      guint n = (guint) atoi (s + 10);
      if (colon && n < A.libraries->len)
        {
          g_auto (GStrv) paths = g_strsplit (colon + 1, "|", -1);
          start_import (A.libraries->pdata[n], paths);
        }
    }
  else if (g_str_has_prefix (s, "attach:") && (guint) atoi (s + 7) < A.libraries->len)
    {
      conversation_toggle_lib (A.conv, ((RagLibrary *) A.libraries->pdata[atoi (s + 7)])->id);
      conv_libs_changed ();
    }
  else if (g_str_has_prefix (s, "assistlib:"))
    {
      guint ai = (guint) atoi (s + 10);
      char *colon = strchr (s + 10, ':');
      if (colon && ai < A.assistants->len && (guint) atoi (colon + 1) < A.libraries->len)
        {
          Assistant *as = A.assistants->pdata[ai];
          g_ptr_array_add (as->libs, g_strdup (((RagLibrary *) A.libraries->pdata[atoi (colon + 1)])->id));
          assistants_save (A.assistants);
          drop_rag_index ();
          refresh_all_docs_ui ();
        }
    }
  else if (g_str_has_prefix (s, "skill:"))
    {
      /* skill:<id>:on|off */
      g_auto (GStrv) parts = g_strsplit (s + 6, ":", 2);
      if (parts[0] && parts[1])
        {
          if (g_str_equal (parts[1], "on"))
            g_hash_table_add (A.skills_on, g_strdup (parts[0]));
          else
            g_hash_table_remove (A.skills_on, parts[0]);
        }
    }
  else if (g_str_equal (s, "scrollprefs") && A.prefs)
    {
      /* Scroll the open preferences down so lower groups can be photographed. */
      GtkWidget *sw = gtk_widget_get_first_child (GTK_WIDGET (A.prefs));
      for (int depth = 0; sw && depth < 30; depth++)
        {
          GtkWidget *found = NULL;
          GtkWidget *kid = gtk_widget_get_first_child (sw);
          while (kid && !found)
            {
              if (GTK_IS_SCROLLED_WINDOW (kid))
                found = kid;
              kid = gtk_widget_get_next_sibling (kid);
            }
          if (found)
            {
              GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (found));
              gtk_adjustment_set_value (adj, gtk_adjustment_get_upper (adj));
              break;
            }
          sw = gtk_widget_get_first_child (sw);
        }
    }
  else if (g_str_equal (s, "caldlg"))
    calendar_ui_open (GTK_WIDGET (A.win));
  else if (g_str_equal (s, "skillsdlg"))
    open_skills_dialog ();
  else if (g_str_equal (s, "libdlg"))
    open_libraries_dialog ();
  else if (g_str_equal (s, "docsmenu"))
    gtk_menu_button_popup (A.docs_button);
  else if (g_str_equal (s, "docsmenuclose"))
    gtk_menu_button_popdown (A.docs_button);
  else if (g_str_has_prefix (s, "rmdoc:"))
    {
      guint n = (guint) atoi (s + 6);
      if (n < A.libraries->len && ((RagLibrary *) A.libraries->pdata[n])->docs->len > 0)
        {
          RagLibrary *lib = A.libraries->pdata[n];
          rag_library_remove_doc (lib, ((RagDoc *) lib->docs->pdata[0])->id);
          drop_rag_index ();
          refresh_all_docs_ui ();
        }
    }
  else if (g_str_has_prefix (s, "dellib:") && (guint) atoi (s + 7) < A.libraries->len)
    {
      forget_library (A.libraries->pdata[atoi (s + 7)]);
      refresh_all_docs_ui ();
    }
  else if (g_str_equal (s, "gallery"))
    open_gallery ();
  else if (g_str_has_prefix (s, "gsearch:") && gallery.dialog)
    gtk_editable_set_text (GTK_EDITABLE (gallery.search), s + 8);
  else if (g_str_has_prefix (s, "galleryadd:"))
    gallery_add (s + 11);
  else if (g_str_equal (s, "editor"))
    open_assistant_editor (A.assistants->len ? A.assistants->pdata[0] : NULL);
  else if (g_str_equal (s, "mkassistant"))
    {
      open_assistant_editor (NULL);
      gtk_editable_set_text (GTK_EDITABLE (editor_current->name), "Nutricionista");
      gtk_editable_set_text (GTK_EDITABLE (editor_current->emoji), "🍎");
      gtk_text_buffer_set_text (gtk_text_view_get_buffer (editor_current->instructions),
                                "Eres un nutricionista amable.", -1);
      on_editor_save (NULL, editor_current);
    }
  else if (g_str_has_prefix (s, "delete:") && A.convs->len > (guint) atoi (s + 7))
    on_delete_chat_response (NULL, "delete", A.convs->pdata[atoi (s + 7)]);
  else if (g_str_has_prefix (s, "open:") && A.convs->len > (guint) atoi (s + 5))
    open_conversation (A.convs->pdata[atoi (s + 5)]);
  else if (g_str_equal (s, "close"))
    {
      g_clear_pointer (&steps, g_strfreev);
      gtk_window_close (A.win);
      return G_SOURCE_REMOVE;
    }
  i++;
  return G_SOURCE_CONTINUE;
}
#endif

int
main (int argc, char **argv)
{
  g_autoptr (AdwApplication) app = adw_application_new (APP_ID, G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);
  g_signal_connect (app, "shutdown", G_CALLBACK (on_shutdown), NULL);
  return g_application_run (G_APPLICATION (app), argc, argv);
}
