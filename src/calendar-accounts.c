#include "calendar-accounts.h"

#include <adwaita.h>
#include <string.h>

#include "calendar-caldav.h"
#include "calendar-subscribe.h"
#include "calendar-ui.h"
#include "calendar.h"
#include "i18n.h"

static AdwDialog *list_dialog;
static GtkWidget *parent_widget;

static void rebuild_list (void);

/* what the server's complaint means for the person who typed the password */
static const char *
friendly (const char *error)
{
  if (error && (strstr (error, "user name or password")))
    return TR ("El servidor no aceptó el usuario o la contraseña. En iCloud hace falta una contraseña para apps (la normal no sirve): créala en appleid.apple.com → Inicio de sesión y seguridad → Contraseñas para apps, y revisa que tu Apple ID tenga la verificación en dos pasos. El usuario es tu correo de Apple ID completo.",
               "The server did not accept the user name or password. iCloud needs an app-specific password (your normal one does not work): make one at appleid.apple.com → Sign-In and Security → App-Specific Passwords, and check that your Apple ID has two-factor authentication. The user is your full Apple ID email.");
  return error ? error : "";
}

static void
tell (const char *heading, const char *body)
{
  AdwDialog *d = adw_alert_dialog_new (heading, body);
  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (d), "ok", TR ("Aceptar", "OK"));
  adw_dialog_present (d, parent_widget);
}

/* ---- choosing which calendars to keep in step ----------------------------- */

typedef struct {
  char      *account_id;
  GPtrArray *remotes;      /* RemoteCalendar*, owned here */
  GPtrArray *checks;       /* GtkCheckButton*, same order */
} Choose;

static void
choose_free (gpointer p)
{
  Choose *c = p;
  for (guint i = 0; i < c->remotes->len; i++)
    {
      RemoteCalendar *r = c->remotes->pdata[i];
      g_free (r->href);
      g_free (r->name);
      g_free (r->color);
      g_free (r->source);
      g_free (r);
    }
  g_ptr_array_unref (c->remotes);
  g_ptr_array_unref (c->checks);
  g_free (c->account_id);
  g_free (c);
}

static void
on_first_sync (const char *error, gpointer data)
{
  (void) data;
  calendar_ui_refresh ();
  rebuild_list ();
  if (error)
    tell (TR ("La sincronización tuvo un problema", "The sync had a problem"), error);
}

static void
on_subscription_loaded (const char *calendar_id, guint events, const char *error, gpointer data)
{
  (void) calendar_id; (void) events; (void) data;
  calendar_ui_refresh ();
  if (error)
    g_message ("calendar: loading a subscribed calendar failed: %s", error);
}

static void
on_choose_response (AdwAlertDialog *dialog, const char *response, gpointer data)
{
  (void) dialog;
  Choose *c = data;
  if (!g_str_equal (response, "keep"))
    return;
  CalAccount *a = caldav_account_find (c->account_id);
  if (!a)
    return;
  GPtrArray *cals = calendar_calendars (calendar_default ());
  for (guint i = 0; i < c->remotes->len; i++)
    {
      RemoteCalendar *r = c->remotes->pdata[i];
      gboolean wanted = gtk_check_button_get_active (GTK_CHECK_BUTTON (c->checks->pdata[i]));
      if (r->source)
        {
          /* an internet calendar subscribed to on the phone: here it is read from its own address */
          const CalCalendar *have_sub = NULL;
          for (guint k = 0; k < cals->len; k++)
            {
              const CalCalendar *l = cals->pdata[k];
              if (l->url && g_str_equal (l->url, r->source))
                have_sub = l;
            }
          if (wanted && !have_sub)
            {
              const char *id = calendar_subscription_add (calendar_default (), r->name,
                                                          r->color ? calendar_color_nearest (r->color) : 5, r->source, 24);
              g_autofree char *keep = g_strdup (id);
              calendar_subscription_fetch (keep, on_subscription_loaded, NULL);
            }
          continue;
        }
      CalCalendar *have = NULL;
      for (guint k = 0; k < cals->len; k++)
        {
          CalCalendar *l = cals->pdata[k];
          if (l->account && g_str_equal (l->account, a->id) && l->href && g_str_equal (l->href, r->href))
            have = l;
        }
      if (wanted && !have)
        caldav_link_calendar (a, r);
      else if (!wanted && have)
        {
          /* no longer kept in step: it stays here as a plain calendar */
          g_free (have->account);
          have->account = NULL;
          g_clear_pointer (&have->href, g_free);
          calendar_calendar_changed (calendar_default ());
        }
    }
  calendar_ui_refresh ();
  caldav_sync_now (on_first_sync, NULL);
}

static void
show_choose (CalAccount *a, GPtrArray *remotes)
{
  Choose *c = g_new0 (Choose, 1);
  c->account_id = g_strdup (a->id);
  c->remotes = g_ptr_array_new ();
  c->checks = g_ptr_array_new ();
  for (guint i = 0; i < remotes->len; i++)
    {
      const RemoteCalendar *src = remotes->pdata[i];
      RemoteCalendar *r = g_new0 (RemoteCalendar, 1);
      r->href = g_strdup (src->href);
      r->name = g_strdup (src->name);
      r->color = g_strdup (src->color);
      r->source = g_strdup (src->source);
      g_ptr_array_add (c->remotes, r);
    }
  AdwDialog *d = adw_alert_dialog_new (TR ("¿Qué calendarios mantengo al día?", "Which calendars should I keep in step?"),
                                       TR ("Los eventos de los calendarios elegidos se copian aquí y los cambios que hagas se envían de vuelta.",
                                           "Events of the chosen calendars are copied here, and changes you make are sent back."));
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  GPtrArray *cals = calendar_calendars (calendar_default ());
  gboolean any_linked = FALSE;
  for (guint k = 0; k < cals->len; k++)
    {
      const CalCalendar *l = cals->pdata[k];
      any_linked |= l->account && g_str_equal (l->account, a->id);
    }
  for (guint i = 0; i < c->remotes->len; i++)
    {
      const RemoteCalendar *r = c->remotes->pdata[i];
      gboolean have = FALSE;
      for (guint k = 0; k < cals->len; k++)
        {
          const CalCalendar *l = cals->pdata[k];
          have |= l->account && g_str_equal (l->account, a->id) && l->href && g_str_equal (l->href, r->href);
        }
      if (r->source)
        {
          for (guint k = 0; k < cals->len; k++)
            {
              const CalCalendar *l = cals->pdata[k];
              have |= l->url && g_str_equal (l->url, r->source);
            }
          any_linked |= have;
        }
      g_autofree char *label = r->source ? g_strdup_printf (TR ("%s (calendario de internet)", "%s (internet calendar)"), r->name) : g_strdup (r->name);
      GtkWidget *check = gtk_check_button_new_with_label (label);
      /* a first connection starts with everything chosen; later visits show what is kept now */
      gtk_check_button_set_active (GTK_CHECK_BUTTON (check), have || !any_linked || r->source != NULL);
      g_ptr_array_add (c->checks, check);
      gtk_box_append (GTK_BOX (box), check);
    }
  adw_alert_dialog_set_extra_child (ADW_ALERT_DIALOG (d), box);
  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (d), "cancel", TR ("Cancelar", "Cancel"));
  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (d), "keep", TR ("Sincronizar", "Sync"));
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (d), "keep", ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (d), "keep");
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (d), "cancel");
  g_object_set_data_full (G_OBJECT (d), "choose", c, choose_free);
  g_signal_connect (d, "response", G_CALLBACK (on_choose_response), c);
  adw_dialog_present (d, parent_widget);
}

typedef struct {
  char *account_id;
  gboolean new_account;   /* remove it again if it cannot be used */
} Find;

static void
on_discovered (const char *error, GPtrArray *calendars, gpointer data)
{
  Find *f = data;
  CalAccount *a = caldav_account_find (f->account_id);
  if (error || !a)
    {
      if (a && f->new_account)
        caldav_account_remove (f->account_id, TRUE);
      rebuild_list ();
      tell (TR ("No se pudo conectar", "Could not connect"), friendly (error));
    }
  else if (!calendars->len)
    {
      if (f->new_account)
        caldav_account_remove (f->account_id, TRUE);
      rebuild_list ();
      tell (TR ("No hay calendarios", "No calendars"), TR ("La cuenta no tiene calendarios de eventos.", "The account has no calendars of events."));
    }
  else
    {
      caldav_accounts_save ();
      rebuild_list ();
      show_choose (a, calendars);
    }
  g_free (f->account_id);
  g_free (f);
}

static void
find_calendars (CalAccount *a, gboolean new_account)
{
  Find *f = g_new0 (Find, 1);
  f->account_id = g_strdup (a->id);
  f->new_account = new_account;
  caldav_discover (a, on_discovered, f);
}

/* ---- adding an account ---------------------------------------------------- */

typedef struct {
  AdwDialog *dialog;
  GtkWidget *preset, *name, *server, *user, *password;
} AddForm;

static void
on_preset (GObject *row, GParamSpec *p, gpointer data)
{
  (void) p;
  AddForm *f = data;
  guint sel = adw_combo_row_get_selected (ADW_COMBO_ROW (row));
  if (sel == 0)
    {
      gtk_editable_set_text (GTK_EDITABLE (f->server), "https://caldav.icloud.com");
      if (!*gtk_editable_get_text (GTK_EDITABLE (f->name)))
        gtk_editable_set_text (GTK_EDITABLE (f->name), "iCloud");
    }
  else if (sel == 1 && !*gtk_editable_get_text (GTK_EDITABLE (f->server)))
    gtk_editable_set_text (GTK_EDITABLE (f->server), "https://tu-servidor/remote.php/dav");
}

static void
on_connect (GtkButton *b, gpointer data)
{
  (void) b;
  AddForm *f = data;
  g_autofree char *server = caldav_check_server_url (gtk_editable_get_text (GTK_EDITABLE (f->server)));
  /* pasted text often carries a space or a line break at the ends */
  g_autofree char *user = g_strstrip (g_strdup (gtk_editable_get_text (GTK_EDITABLE (f->user))));
  g_autofree char *typed = g_strstrip (g_strdup (gtk_editable_get_text (GTK_EDITABLE (f->password))));
  const char *name = gtk_editable_get_text (GTK_EDITABLE (f->name));
  if (!server)
    {
      tell (TR ("La dirección no es válida", "That address is not valid"),
            TR ("Usa una dirección https://. Para iCloud: https://caldav.icloud.com", "Use an https:// address. For iCloud: https://caldav.icloud.com"));
      return;
    }
  if (!*user || !*typed)
    {
      tell (TR ("Faltan datos", "Something is missing"), TR ("Escribe el usuario y la contraseña.", "Enter the user name and the password."));
      return;
    }
  /* Apple's password for apps has a known shape; a normal password never works, so say so before trying */
  if (caldav_is_icloud (server) && !caldav_looks_like_app_password (typed))
    {
      tell (TR ("Eso no parece una contraseña para apps", "That does not look like an app-specific password"),
            TR ("iCloud no acepta tu contraseña normal. Una contraseña para apps se ve así: abcd-efgh-ijkl-mnop. Créala en appleid.apple.com → Inicio de sesión y seguridad → Contraseñas para apps (necesitas la verificación en dos pasos) y pégala aquí.",
                "iCloud does not accept your normal password. An app-specific password looks like this: abcd-efgh-ijkl-mnop. Make one at appleid.apple.com → Sign-In and Security → App-Specific Passwords (you need two-factor authentication) and paste it here."));
      return;
    }
  /* the password is shown to you in groups; the server wants it without spaces */
  GString *clean = g_string_new (NULL);
  for (const char *c = typed; *c; c++)
    if (!(*c == ' ' && caldav_is_icloud (server)))
      g_string_append_c (clean, *c);
  CalAccount *a = caldav_account_add (*name ? name : "CalDAV", server, user, clean->str);
  memset (clean->str, 0, clean->len);
  g_string_free (clean, TRUE);
  memset (typed, 0, strlen (typed));
  adw_dialog_close (f->dialog);
  find_calendars (a, TRUE);
}

static void
open_add (void)
{
  AddForm *f = g_new0 (AddForm, 1);
  f->dialog = adw_dialog_new ();
  adw_dialog_set_title (f->dialog, TR ("Añadir una cuenta", "Add an account"));
  adw_dialog_set_content_width (f->dialog, 480);
  adw_dialog_set_content_height (f->dialog, 640);
  g_object_set_data_full (G_OBJECT (f->dialog), "form", f, g_free);

  GtkWidget *page = adw_preferences_page_new ();
  AdwPreferencesGroup *help = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (help, TR ("iPhone y iCloud", "iPhone and iCloud"));
  adw_preferences_group_set_description (help,
    TR ("Apple no deja usar tu contraseña normal. Crea una contraseña para apps: entra en appleid.apple.com, abre «Inicio de sesión y seguridad», luego «Contraseñas para apps», crea una con el nombre «Calendario» y pégala abajo. Tu usuario es tu correo de Apple ID. Se puede revocar cuando quieras.",
        "Apple does not allow your normal password. Make an app-specific password: sign in at appleid.apple.com, open “Sign-In and Security”, then “App-Specific Passwords”, create one called “Calendar” and paste it below. Your user is your Apple ID email. You can revoke it at any time."));
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), help);

  AdwPreferencesGroup *form = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  f->preset = adw_combo_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (f->preset), TR ("Tipo de cuenta", "Account type"));
  const char *presets[] = { "iCloud (iPhone, iPad, Mac)", "Nextcloud", TR ("Otro servidor CalDAV", "Another CalDAV server"), NULL };
  g_autoptr (GtkStringList) preset_list = gtk_string_list_new (presets);
  adw_combo_row_set_model (ADW_COMBO_ROW (f->preset), G_LIST_MODEL (preset_list));
  adw_preferences_group_add (form, f->preset);
  f->name = adw_entry_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (f->name), TR ("Nombre", "Name"));
  gtk_editable_set_text (GTK_EDITABLE (f->name), "iCloud");
  adw_preferences_group_add (form, f->name);
  f->server = adw_entry_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (f->server), TR ("Servidor", "Server"));
  gtk_editable_set_text (GTK_EDITABLE (f->server), "https://caldav.icloud.com");
  adw_preferences_group_add (form, f->server);
  f->user = adw_entry_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (f->user), TR ("Usuario", "User"));
  adw_preferences_group_add (form, f->user);
  f->password = adw_password_entry_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (f->password), TR ("Contraseña de app", "App password"));
  adw_preferences_group_add (form, f->password);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), form);
  g_signal_connect (f->preset, "notify::selected", G_CALLBACK (on_preset), f);

  AdwPreferencesGroup *note = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_description (note, TR ("La contraseña se guarda en un archivo que solo puedes leer tú, en ~/.config/calendar/. Solo se contacta ese servidor.",
                                                   "The password is kept in a file only you can read, in ~/.config/calendar/. Only that server is contacted."));
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), note);

  GtkWidget *header = adw_header_bar_new ();
  adw_header_bar_set_show_end_title_buttons (ADW_HEADER_BAR (header), FALSE);
  adw_header_bar_set_show_start_title_buttons (ADW_HEADER_BAR (header), FALSE);
  GtkWidget *cancel = gtk_button_new_with_label (TR ("Cancelar", "Cancel"));
  g_signal_connect_swapped (cancel, "clicked", G_CALLBACK (adw_dialog_close), f->dialog);
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), cancel);
  GtkWidget *connect = gtk_button_new_with_label (TR ("Conectar", "Connect"));
  gtk_widget_add_css_class (connect, "suggested-action");
  g_signal_connect (connect, "clicked", G_CALLBACK (on_connect), f);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), connect);
  GtkWidget *tv = adw_toolbar_view_new ();
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (tv), header);
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (tv), page);
  adw_dialog_set_child (f->dialog, tv);
  adw_dialog_present (f->dialog, parent_widget);
}

/* ---- the list ------------------------------------------------------------- */

static void
on_choose_calendars (GtkButton *b, gpointer data)
{
  (void) b;
  CalAccount *a = caldav_account_find (data);
  if (a)
    find_calendars (a, FALSE);
}

static void
on_remove_response (AdwAlertDialog *dialog, const char *response, gpointer data)
{
  (void) dialog;
  if (g_str_equal (response, "keep"))
    caldav_account_remove (data, FALSE);
  else if (g_str_equal (response, "delete"))
    caldav_account_remove (data, TRUE);
  else
    return;
  calendar_ui_refresh ();
  rebuild_list ();
}

static void
on_remove (GtkButton *b, gpointer data)
{
  (void) b;
  AdwDialog *d = adw_alert_dialog_new (TR ("¿Quitar esta cuenta?", "Remove this account?"),
                                       TR ("Los eventos no se borran del servidor ni de tu iPhone.", "The events are not deleted from the server or your iPhone."));
  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (d), "cancel", TR ("Cancelar", "Cancel"));
  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (d), "delete", TR ("Quitar y borrar aquí", "Remove and delete here"));
  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (d), "keep", TR ("Quitar y conservar aquí", "Remove and keep here"));
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (d), "delete", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (d), "keep");
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (d), "cancel");
  g_signal_connect_data (d, "response", G_CALLBACK (on_remove_response), g_strdup (data), (GClosureNotify) (void (*) (void)) g_free, 0);
  adw_dialog_present (d, parent_widget);
}

static void
on_sync_clicked (GtkButton *b, gpointer data)
{
  (void) b; (void) data;
  caldav_sync_now (on_first_sync, NULL);
}

static void
on_add_clicked (GtkButton *b, gpointer data)
{
  (void) b; (void) data;
  open_add ();
}

static char *
status_text (const CalAccount *a)
{
  if (a->last_error)
    return g_strdup_printf ("%s %s", TR ("Error:", "Error:"), a->last_error);
  if (!a->last_sync)
    return g_strdup (TR ("Todavía no se ha sincronizado", "Not synced yet"));
  gint64 ago = g_get_real_time () / G_USEC_PER_SEC - a->last_sync;
  if (ago < 90)
    return g_strdup (TR ("Sincronizado hace un momento", "Synced a moment ago"));
  if (ago < 3600)
    return g_strdup_printf (TR ("Sincronizado hace %d min", "Synced %d min ago"), (int) (ago / 60));
  if (ago < 86400)
    return g_strdup_printf (TR ("Sincronizado hace %d h", "Synced %d h ago"), (int) (ago / 3600));
  return g_strdup_printf (TR ("Sincronizado hace %d días", "Synced %d days ago"), (int) (ago / 86400));
}

static GtkWidget *list_box;

static void
rebuild_list (void)
{
  if (!list_dialog || !list_box)
    return;
  GtkWidget *kid;
  while ((kid = gtk_widget_get_first_child (list_box)))
    gtk_box_remove (GTK_BOX (list_box), kid);
  const GPtrArray *accounts = caldav_accounts ();
  if (!accounts->len)
    {
      GtkWidget *empty = gtk_label_new (TR ("Aún no hay cuentas. Añade tu iCloud para ver aquí los eventos de tu iPhone y que lo que apuntes aparezca allá.",
                                            "No accounts yet. Add your iCloud to see your iPhone's events here, and have what you add show up there."));
      gtk_label_set_wrap (GTK_LABEL (empty), TRUE);
      gtk_widget_add_css_class (empty, "dim-label");
      gtk_label_set_xalign (GTK_LABEL (empty), 0);
      gtk_box_append (GTK_BOX (list_box), empty);
    }
  AdwPreferencesGroup *group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  for (guint i = 0; i < accounts->len; i++)
    {
      const CalAccount *a = accounts->pdata[i];
      GtkWidget *row = adw_action_row_new ();
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), a->name);
      g_autofree char *status = status_text (a);
      g_autofree char *sub = g_strdup_printf ("%s\n%s", a->user, status);
      adw_action_row_set_subtitle (ADW_ACTION_ROW (row), sub);
      GtkWidget *cals = gtk_button_new_with_label (TR ("Calendarios", "Calendars"));
      gtk_widget_set_valign (cals, GTK_ALIGN_CENTER);
      g_signal_connect_data (cals, "clicked", G_CALLBACK (on_choose_calendars), g_strdup (a->id), (GClosureNotify) (void (*) (void)) g_free, 0);
      adw_action_row_add_suffix (ADW_ACTION_ROW (row), cals);
      GtkWidget *rm = gtk_button_new_with_label (TR ("Quitar", "Remove"));
      gtk_widget_set_valign (rm, GTK_ALIGN_CENTER);
      gtk_widget_add_css_class (rm, "destructive-action");
      g_signal_connect_data (rm, "clicked", G_CALLBACK (on_remove), g_strdup (a->id), (GClosureNotify) (void (*) (void)) g_free, 0);
      adw_action_row_add_suffix (ADW_ACTION_ROW (row), rm);
      adw_preferences_group_add (group, row);
    }
  if (accounts->len)
    gtk_box_append (GTK_BOX (list_box), GTK_WIDGET (group));
  else
    g_object_unref (g_object_ref_sink (group));
  GtkWidget *buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *add = gtk_button_new_with_label (TR ("Añadir una cuenta…", "Add an account…"));
  gtk_widget_add_css_class (add, "suggested-action");
  g_signal_connect (add, "clicked", G_CALLBACK (on_add_clicked), NULL);
  gtk_box_append (GTK_BOX (buttons), add);
  if (accounts->len)
    {
      GtkWidget *sync = gtk_button_new_with_label (TR ("Sincronizar ahora", "Sync now"));
      g_signal_connect (sync, "clicked", G_CALLBACK (on_sync_clicked), NULL);
      gtk_box_append (GTK_BOX (buttons), sync);
    }
  gtk_box_append (GTK_BOX (list_box), buttons);
}

static void
on_list_closed (AdwDialog *d, gpointer data)
{
  (void) d; (void) data;
  list_dialog = NULL;
  list_box = NULL;
}

void
calendar_accounts_open (GtkWidget *parent)
{
  parent_widget = parent;
  if (list_dialog)
    return;
  list_dialog = adw_dialog_new ();
  adw_dialog_set_title (list_dialog, TR ("Cuentas", "Accounts"));
  adw_dialog_set_content_width (list_dialog, 520);
  adw_dialog_set_content_height (list_dialog, 460);
  g_signal_connect (list_dialog, "closed", G_CALLBACK (on_list_closed), NULL);
  list_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start (list_box, 16);
  gtk_widget_set_margin_end (list_box, 16);
  gtk_widget_set_margin_top (list_box, 8);
  gtk_widget_set_margin_bottom (list_box, 16);
  GtkWidget *scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), list_box);
  GtkWidget *tv = adw_toolbar_view_new ();
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (tv), adw_header_bar_new ());
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (tv), scroll);
  adw_dialog_set_child (list_dialog, tv);
  rebuild_list ();
  adw_dialog_present (list_dialog, parent);
}
