#include "calendar-ui.h"

#include <adwaita.h>
#include <string.h>

#include "calendar.h"
#include "i18n.h"
#include "selftest.h"

#define N_COLORS 6
#define CHIPS_PER_DAY 3

static struct {
  AdwDialog *dialog;  /* only when shown as a dialog */
  GtkWidget *view;    /* the calendar itself, wherever it lives */
  GtkWidget *grid;
  GtkWidget *month_label;
  GtkWidget *weekdays;
  GtkWidget *agenda_title;
  GtkWidget *agenda_list;
  GtkWidget *agenda_empty;
  gint64     month;     /* local midnight of the 1st of the shown month */
  gint64     selected;  /* local midnight of the chosen day */
} U;

/* ---- names -------------------------------------------------------------- */

static const char *months_es[] = { "enero", "febrero", "marzo", "abril", "mayo", "junio", "julio", "agosto",
                                   "septiembre", "octubre", "noviembre", "diciembre" };
static const char *months_en[] = { "January", "February", "March", "April", "May", "June", "July", "August",
                                   "September", "October", "November", "December" };
/* index 0 = Monday */
static const char *days_es[] = { "lun", "mar", "mié", "jue", "vie", "sáb", "dom" };
static const char *days_en[] = { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };
static const char *days_long_es[] = { "lunes", "martes", "miércoles", "jueves", "viernes", "sábado", "domingo" };
static const char *days_long_en[] = { "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday" };

static const char *
month_name (int m)
{
  return i18n_lang () == LANG_EN ? months_en[m - 1] : months_es[m - 1];
}

/* Spanish weeks start on Monday, English ones on Sunday. Returns 0 for Monday, 6 for Sunday. */
static int
week_start (void)
{
  return i18n_lang () == LANG_EN ? 6 : 0;
}

static GDateTime *
local_dt (gint64 t)
{
  return g_date_time_new_from_unix_local (t);
}

static char *
format_hm (gint64 t)
{
  g_autoptr (GDateTime) d = local_dt (t);
  return g_date_time_format (d, "%H:%M");
}

static char *
format_day (gint64 t)
{
  g_autoptr (GDateTime) d = local_dt (t);
  int dow = g_date_time_get_day_of_week (d) - 1;
  int day = g_date_time_get_day_of_month (d);
  int month = g_date_time_get_month (d);
  if (i18n_lang () == LANG_EN)
    return g_strdup_printf ("%s, %s %d", days_long_en[dow], months_en[month - 1], day);
  return g_strdup_printf ("%s %d de %s", days_long_es[dow], day, months_es[month - 1]);
}

static gint64
month_start (gint64 t)
{
  g_autoptr (GDateTime) d = local_dt (t);
  g_autoptr (GDateTime) first = g_date_time_new_local (g_date_time_get_year (d), g_date_time_get_month (d), 1, 0, 0, 0);
  return g_date_time_to_unix (first);
}

static gint64
add_months (gint64 t, int n)
{
  g_autoptr (GDateTime) d = local_dt (t);
  g_autoptr (GDateTime) r = g_date_time_add_months (d, n);
  return g_date_time_to_unix (r);
}

static guint
color_of (const CalEvent *ev)
{
  return g_str_hash (ev->id) % N_COLORS;
}

static const char *
repeat_label (CalRepeat r)
{
  switch (r)
    {
    case CAL_REPEAT_DAILY:   return TR ("Cada día", "Every day");
    case CAL_REPEAT_WEEKLY:  return TR ("Cada semana", "Every week");
    case CAL_REPEAT_MONTHLY: return TR ("Cada mes", "Every month");
    case CAL_REPEAT_YEARLY:  return TR ("Cada año", "Every year");
    default:                 return NULL;
    }
}

/* ---- event editor ------------------------------------------------------- */

typedef struct {
  AdwDialog *dialog;
  char      *id;          /* NULL when creating */
  gint64     day;         /* local midnight of the chosen day */
  gint64     until;
  GtkWidget *title, *notes, *all_day, *date_button, *calendar, *times_row;
  GtkWidget *start_h, *start_m, *end_h, *end_m, *repeat;
} Editor;

static void
editor_free (AdwDialog *dialog, gpointer data)
{
  (void) dialog;
  Editor *e = data;
  g_free (e->id);
  g_free (e);
}

static void
editor_update_date_label (Editor *e)
{
  g_autofree char *text = format_day (e->day);
  gtk_menu_button_set_label (GTK_MENU_BUTTON (e->date_button), text);
}

static void
on_editor_day_selected (GtkCalendar *calendar, gpointer data)
{
  Editor *e = data;
  g_autoptr (GDateTime) d = gtk_calendar_get_date (calendar);
  g_autoptr (GDateTime) m = g_date_time_new_local (g_date_time_get_year (d), g_date_time_get_month (d),
                                                    g_date_time_get_day_of_month (d), 0, 0, 0);
  e->day = g_date_time_to_unix (m);
  editor_update_date_label (e);
}

static void
on_editor_all_day (GObject *row, GParamSpec *pspec, gpointer data)
{
  (void) pspec;
  Editor *e = data;
  gtk_widget_set_visible (e->times_row, !adw_switch_row_get_active (ADW_SWITCH_ROW (row)));
}

static gint64
editor_time (Editor *e, GtkWidget *h, GtkWidget *m)
{
  g_autoptr (GDateTime) d = local_dt (e->day);
  g_autoptr (GDateTime) t = g_date_time_new_local (g_date_time_get_year (d), g_date_time_get_month (d),
                                                    g_date_time_get_day_of_month (d),
                                                    (int) gtk_spin_button_get_value (GTK_SPIN_BUTTON (h)),
                                                    (int) gtk_spin_button_get_value (GTK_SPIN_BUTTON (m)), 0);
  return g_date_time_to_unix (t);
}

static void
on_editor_save (GtkButton *button, gpointer data)
{
  (void) button;
  Editor *e = data;
  gboolean all_day = adw_switch_row_get_active (ADW_SWITCH_ROW (e->all_day));
  gint64 start = all_day ? e->day : editor_time (e, e->start_h, e->start_m);
  gint64 end = all_day ? calendar_day_next (e->day) : editor_time (e, e->end_h, e->end_m);
  if (end <= start)
    end = start + 900;

  const char *title = gtk_editable_get_text (GTK_EDITABLE (e->title));
  CalEvent *ev = calendar_event_new (*title ? title : TR ("Sin título", "Untitled"), start, end, all_day);
  g_free (ev->notes);
  ev->notes = g_strdup (gtk_editable_get_text (GTK_EDITABLE (e->notes)));
  ev->repeat = (CalRepeat) adw_combo_row_get_selected (ADW_COMBO_ROW (e->repeat));
  ev->until = ev->repeat == CAL_REPEAT_NONE ? 0 : e->until;

  if (e->id)
    {
      g_free (ev->id);
      ev->id = g_strdup (e->id);
      calendar_update (calendar_default (), ev);
    }
  else
    calendar_add (calendar_default (), ev);

  U.selected = e->day;
  U.month = month_start (e->day);
  adw_dialog_close (e->dialog);
  calendar_ui_refresh ();
}

static void
on_delete_response (AdwAlertDialog *dialog, const char *response, gpointer data)
{
  (void) dialog;
  Editor *e = data;
  if (g_str_equal (response, "delete"))
    {
      calendar_remove (calendar_default (), e->id);
      adw_dialog_close (e->dialog);
      calendar_ui_refresh ();
    }
}

static void
on_editor_delete (GtkButton *button, gpointer data)
{
  (void) button;
  Editor *e = data;
  AdwDialog *ask = adw_alert_dialog_new (TR ("¿Borrar este evento?", "Delete this event?"),
                                         TR ("No se puede deshacer.", "This cannot be undone."));
  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (ask), "cancel", TR ("Cancelar", "Cancel"));
  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (ask), "delete", TR ("Borrar", "Delete"));
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (ask), "delete", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (ask), "cancel");
  g_signal_connect (ask, "response", G_CALLBACK (on_delete_response), e);
  adw_dialog_present (ask, GTK_WIDGET (e->dialog));
}

static GtkWidget *
time_spins (GtkWidget **hour, GtkWidget **minute, int h, int m)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_set_valign (box, GTK_ALIGN_CENTER);
  *hour = gtk_spin_button_new_with_range (0, 23, 1);
  *minute = gtk_spin_button_new_with_range (0, 59, 5);
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (*hour), h);
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (*minute), m);
  gtk_spin_button_set_wrap (GTK_SPIN_BUTTON (*hour), TRUE);
  gtk_spin_button_set_wrap (GTK_SPIN_BUTTON (*minute), TRUE);
  gtk_box_append (GTK_BOX (box), *hour);
  gtk_box_append (GTK_BOX (box), gtk_label_new (":"));
  gtk_box_append (GTK_BOX (box), *minute);
  return box;
}

static void
open_editor (const CalEvent *existing, gint64 day)
{
  Editor *e = g_new0 (Editor, 1);
  e->id = existing ? g_strdup (existing->id) : NULL;
  e->day = existing ? calendar_day_start (existing->start) : day;
  e->until = existing ? existing->until : 0;

  e->dialog = adw_dialog_new ();
  adw_dialog_set_title (e->dialog, existing ? TR ("Editar evento", "Edit event") : TR ("Nuevo evento", "New event"));
  adw_dialog_set_content_width (e->dialog, 460);
  g_signal_connect (e->dialog, "closed", G_CALLBACK (editor_free), e);

  GtkWidget *page = adw_preferences_page_new ();
  AdwPreferencesGroup *main = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());

  e->title = adw_entry_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (e->title), TR ("Título", "Title"));
  if (existing)
    gtk_editable_set_text (GTK_EDITABLE (e->title), existing->title);
  adw_preferences_group_add (main, e->title);

  e->all_day = adw_switch_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (e->all_day), TR ("Todo el día", "All day"));
  adw_switch_row_set_active (ADW_SWITCH_ROW (e->all_day), existing && existing->all_day);
  adw_preferences_group_add (main, e->all_day);

  GtkWidget *date_row = adw_action_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (date_row), TR ("Fecha", "Date"));
  e->date_button = gtk_menu_button_new ();
  gtk_widget_set_valign (e->date_button, GTK_ALIGN_CENTER);
  e->calendar = gtk_calendar_new ();
  GtkWidget *pop = gtk_popover_new ();
  gtk_popover_set_child (GTK_POPOVER (pop), e->calendar);
  gtk_menu_button_set_popover (GTK_MENU_BUTTON (e->date_button), pop);
  adw_action_row_add_suffix (ADW_ACTION_ROW (date_row), e->date_button);
  adw_preferences_group_add (main, date_row);
  {
    g_autoptr (GDateTime) d = local_dt (e->day);
    /* gtk_calendar_set_date only exists from GTK 4.20; Ubuntu 24.04 ships 4.14 */
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    gtk_calendar_select_day (GTK_CALENDAR (e->calendar), d);
    G_GNUC_END_IGNORE_DEPRECATIONS
  }
  g_signal_connect (e->calendar, "day-selected", G_CALLBACK (on_editor_day_selected), e);
  editor_update_date_label (e);

  /* a new event starts at the next full hour */
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  int start_h = (g_date_time_get_hour (now) + 1) % 24, start_m = 0, end_h = (start_h + 1) % 24, end_m = 0;
  if (existing && !existing->all_day)
    {
      g_autoptr (GDateTime) s = local_dt (existing->start);
      g_autoptr (GDateTime) en = local_dt (existing->end);
      start_h = g_date_time_get_hour (s);
      start_m = g_date_time_get_minute (s);
      end_h = g_date_time_get_hour (en);
      end_m = g_date_time_get_minute (en);
    }
  e->times_row = adw_action_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (e->times_row), TR ("Hora", "Time"));
  GtkWidget *start_box = time_spins (&e->start_h, &e->start_m, start_h, start_m);
  GtkWidget *end_box = time_spins (&e->end_h, &e->end_m, end_h, end_m);
  GtkWidget *times = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_append (GTK_BOX (times), start_box);
  gtk_box_append (GTK_BOX (times), gtk_label_new ("–"));
  gtk_box_append (GTK_BOX (times), end_box);
  adw_action_row_add_suffix (ADW_ACTION_ROW (e->times_row), times);
  adw_preferences_group_add (main, e->times_row);
  gtk_widget_set_visible (e->times_row, !(existing && existing->all_day));
  g_signal_connect (e->all_day, "notify::active", G_CALLBACK (on_editor_all_day), e);

  e->repeat = adw_combo_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (e->repeat), TR ("Repetir", "Repeat"));
  const char *repeat_items[] = { TR ("Nunca", "Never"), TR ("Cada día", "Every day"), TR ("Cada semana", "Every week"),
                                 TR ("Cada mes", "Every month"), TR ("Cada año", "Every year"), NULL };
  g_autoptr (GtkStringList) list = gtk_string_list_new (repeat_items);
  adw_combo_row_set_model (ADW_COMBO_ROW (e->repeat), G_LIST_MODEL (list));
  adw_combo_row_set_selected (ADW_COMBO_ROW (e->repeat), existing ? (guint) existing->repeat : 0);
  adw_preferences_group_add (main, e->repeat);

  e->notes = adw_entry_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (e->notes), TR ("Notas", "Notes"));
  if (existing)
    gtk_editable_set_text (GTK_EDITABLE (e->notes), existing->notes);
  adw_preferences_group_add (main, e->notes);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), main);

  if (existing)
    {
      AdwPreferencesGroup *danger = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
      GtkWidget *del = gtk_button_new_with_label (TR ("Borrar evento", "Delete event"));
      gtk_widget_add_css_class (del, "destructive-action");
      gtk_widget_set_halign (del, GTK_ALIGN_CENTER);
      g_signal_connect (del, "clicked", G_CALLBACK (on_editor_delete), e);
      adw_preferences_group_add (danger, del);
      adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), danger);
    }

  GtkWidget *header = adw_header_bar_new ();
  adw_header_bar_set_show_end_title_buttons (ADW_HEADER_BAR (header), FALSE);
  adw_header_bar_set_show_start_title_buttons (ADW_HEADER_BAR (header), FALSE);
  GtkWidget *cancel = gtk_button_new_with_label (TR ("Cancelar", "Cancel"));
  g_signal_connect_swapped (cancel, "clicked", G_CALLBACK (adw_dialog_close), e->dialog);
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), cancel);
  GtkWidget *save = gtk_button_new_with_label (TR ("Guardar", "Save"));
  gtk_widget_add_css_class (save, "suggested-action");
  g_signal_connect (save, "clicked", G_CALLBACK (on_editor_save), e);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), save);

  GtkWidget *tv = adw_toolbar_view_new ();
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (tv), header);
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (tv), page);
  adw_dialog_set_child (e->dialog, tv);
  adw_dialog_set_default_widget (e->dialog, save);
  adw_dialog_present (e->dialog, GTK_WIDGET (U.view));
}

/* ---- agenda ------------------------------------------------------------- */

static char *
time_text (const CalOccurrence *o)
{
  if (o->event->all_day)
    return g_strdup (TR ("Todo el día", "All day"));
  g_autofree char *a = format_hm (o->start);
  g_autofree char *b = format_hm (o->end);
  return g_strdup_printf ("%s – %s", a, b);
}

static void
on_agenda_row_activated (AdwActionRow *row, gpointer data)
{
  (void) data;
  const char *id = g_object_get_data (G_OBJECT (row), "event-id");
  const CalEvent *ev = calendar_find (calendar_default (), id);
  if (ev)
    open_editor (ev, U.selected);
}

static void
rebuild_agenda (void)
{
  g_autofree char *title = format_day (U.selected);
  gtk_label_set_text (GTK_LABEL (U.agenda_title), title);
  gtk_list_box_remove_all (GTK_LIST_BOX (U.agenda_list));

  GArray *occ = calendar_occurrences (calendar_default (), U.selected, calendar_day_next (U.selected));
  for (guint i = 0; i < occ->len; i++)
    {
      const CalOccurrence *o = &g_array_index (occ, CalOccurrence, i);
      GtkWidget *row = adw_action_row_new ();
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), o->event->title);
      g_autofree char *when = time_text (o);
      const char *rep = repeat_label (o->event->repeat);
      g_autofree char *sub = rep ? g_strdup_printf ("%s · %s", when, rep) : g_strdup (when);
      adw_action_row_set_subtitle (ADW_ACTION_ROW (row), sub);
      GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
      gtk_widget_add_css_class (bar, "cal-bar");
      g_autofree char *cls = g_strdup_printf ("cal-c%u", color_of (o->event));
      gtk_widget_add_css_class (bar, cls);
      adw_action_row_add_prefix (ADW_ACTION_ROW (row), bar);
      gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), TRUE);
      g_object_set_data_full (G_OBJECT (row), "event-id", g_strdup (o->event->id), g_free);
      g_signal_connect (row, "activated", G_CALLBACK (on_agenda_row_activated), NULL);
      gtk_list_box_append (GTK_LIST_BOX (U.agenda_list), row);
    }
  gtk_widget_set_visible (U.agenda_list, occ->len > 0);
  gtk_widget_set_visible (U.agenda_empty, occ->len == 0);
  g_array_free (occ, TRUE);
}

/* ---- month grid --------------------------------------------------------- */

static void
on_day_clicked (GtkButton *button, gpointer data)
{
  (void) data;
  gint64 *day = g_object_get_data (G_OBJECT (button), "day");
  U.selected = *day;
  U.month = month_start (*day);
  calendar_ui_refresh ();
}

static GtkWidget *
make_chip (const CalOccurrence *o)
{
  g_autofree char *hm = o->event->all_day ? NULL : format_hm (o->start);
  g_autofree char *text = hm ? g_strdup_printf ("%s %s", hm, o->event->title) : g_strdup (o->event->title);
  GtkWidget *chip = gtk_label_new (text);
  gtk_label_set_xalign (GTK_LABEL (chip), 0);
  gtk_label_set_ellipsize (GTK_LABEL (chip), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class (chip, "cal-chip");
  g_autofree char *cls = g_strdup_printf ("cal-c%u", color_of (o->event));
  gtk_widget_add_css_class (chip, cls);
  return chip;
}

static void
rebuild_grid (void)
{
  GtkWidget *kid;
  while ((kid = gtk_widget_get_first_child (U.grid)))
    gtk_grid_remove (GTK_GRID (U.grid), kid);

  g_autoptr (GDateTime) first = local_dt (U.month);
  g_autofree char *title = g_strdup_printf ("%s %d", month_name (g_date_time_get_month (first)), g_date_time_get_year (first));
  title[0] = (char) g_ascii_toupper (title[0]);
  gtk_label_set_text (GTK_LABEL (U.month_label), title);

  int offset = (g_date_time_get_day_of_week (first) - 1 - week_start () + 7) % 7;
  g_autoptr (GDateTime) cell0 = g_date_time_add_days (first, -offset);
  gint64 grid_start = g_date_time_to_unix (cell0);
  g_autoptr (GDateTime) after = g_date_time_add_days (cell0, 42);
  GArray *occ = calendar_occurrences (calendar_default (), grid_start, g_date_time_to_unix (after));

  gint64 today = calendar_day_start (g_get_real_time () / G_USEC_PER_SEC);
  for (int i = 0; i < 42; i++)
    {
      g_autoptr (GDateTime) d = g_date_time_add_days (cell0, i);
      gint64 day = g_date_time_to_unix (d);
      gint64 next = calendar_day_next (day);

      GtkWidget *button = gtk_button_new ();
      gtk_widget_add_css_class (button, "cal-day");
      if (g_date_time_get_month (d) != g_date_time_get_month (first))
        gtk_widget_add_css_class (button, "other");
      if (day == U.selected)
        gtk_widget_add_css_class (button, "selected");
      gtk_widget_set_hexpand (button, TRUE);
      gtk_widget_set_vexpand (button, TRUE);

      GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
      g_autofree char *num = g_strdup_printf ("%d", g_date_time_get_day_of_month (d));
      GtkWidget *number = gtk_label_new (num);
      gtk_widget_add_css_class (number, "cal-num");
      if (day == today)
        gtk_widget_add_css_class (number, "today");
      gtk_widget_set_halign (number, GTK_ALIGN_START);
      gtk_box_append (GTK_BOX (box), number);

      guint shown = 0, total = 0;
      for (guint k = 0; k < occ->len; k++)
        {
          const CalOccurrence *o = &g_array_index (occ, CalOccurrence, k);
          if (o->start >= next || o->end <= day)
            continue;
          total++;
          if (shown < CHIPS_PER_DAY)
            {
              gtk_box_append (GTK_BOX (box), make_chip (o));
              shown++;
            }
        }
      if (total > shown)
        {
          g_autofree char *more = g_strdup_printf (TR ("+%u más", "+%u more"), total - shown);
          GtkWidget *l = gtk_label_new (more);
          gtk_widget_add_css_class (l, "cal-more");
          gtk_widget_set_halign (l, GTK_ALIGN_START);
          gtk_box_append (GTK_BOX (box), l);
        }
      gtk_button_set_child (GTK_BUTTON (button), box);

      gint64 *keep = g_memdup2 (&day, sizeof day);
      g_object_set_data_full (G_OBJECT (button), "day", keep, g_free);
      g_signal_connect (button, "clicked", G_CALLBACK (on_day_clicked), NULL);
      gtk_grid_attach (GTK_GRID (U.grid), button, i % 7, i / 7, 1, 1);
    }
  g_array_free (occ, TRUE);

  /* weekday names follow the week's first day */
  GtkWidget *w;
  while ((w = gtk_widget_get_first_child (U.weekdays)))
    gtk_box_remove (GTK_BOX (U.weekdays), w);
  for (int i = 0; i < 7; i++)
    {
      int dow = (week_start () + i) % 7;
      GtkWidget *l = gtk_label_new (i18n_lang () == LANG_EN ? days_en[dow] : days_es[dow]);
      gtk_widget_add_css_class (l, "cal-weekday");
      gtk_widget_set_hexpand (l, TRUE);
      gtk_box_append (GTK_BOX (U.weekdays), l);
    }
  gtk_box_set_homogeneous (GTK_BOX (U.weekdays), TRUE);
}

void
calendar_ui_refresh (void)
{
  if (!U.view)
    return;
  rebuild_grid ();
  rebuild_agenda ();
}

/* ---- window ------------------------------------------------------------- */

static void
on_prev (GtkButton *b, gpointer d)
{
  (void) b; (void) d;
  U.month = add_months (U.month, -1);
  calendar_ui_refresh ();
}

static void
on_next (GtkButton *b, gpointer d)
{
  (void) b; (void) d;
  U.month = add_months (U.month, 1);
  calendar_ui_refresh ();
}

static void
on_today (GtkButton *b, gpointer d)
{
  (void) b; (void) d;
  U.selected = calendar_day_start (g_get_real_time () / G_USEC_PER_SEC);
  U.month = month_start (U.selected);
  calendar_ui_refresh ();
}

static void
on_new_event (GtkButton *b, gpointer d)
{
  (void) b; (void) d;
  open_editor (NULL, U.selected);
}

static void
on_closed (AdwDialog *dialog, gpointer data)
{
  (void) dialog; (void) data;
  memset (&U, 0, sizeof U);
}

void
calendar_ui_close (void)
{
  if (U.dialog)
    adw_dialog_force_close (U.dialog);
}

void
calendar_ui_forget (void)
{
  memset (&U, 0, sizeof U);
}

GtkWidget *
calendar_ui_view_new (void)
{
  U.selected = calendar_day_start (g_get_real_time () / G_USEC_PER_SEC);
  U.month = month_start (U.selected);
  GtkWidget *header = adw_header_bar_new ();
  GtkWidget *nav = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (nav, "linked");
  GtkWidget *prev = gtk_button_new_from_icon_name ("go-previous-symbolic");
  GtkWidget *next = gtk_button_new_from_icon_name ("go-next-symbolic");
  gtk_widget_set_tooltip_text (prev, TR ("Mes anterior", "Previous month"));
  gtk_widget_set_tooltip_text (next, TR ("Mes siguiente", "Next month"));
  g_signal_connect (prev, "clicked", G_CALLBACK (on_prev), NULL);
  g_signal_connect (next, "clicked", G_CALLBACK (on_next), NULL);
  gtk_box_append (GTK_BOX (nav), prev);
  gtk_box_append (GTK_BOX (nav), next);
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), nav);
  GtkWidget *today = gtk_button_new_with_label (TR ("Hoy", "Today"));
  g_signal_connect (today, "clicked", G_CALLBACK (on_today), NULL);
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), today);
  U.month_label = gtk_label_new ("");
  gtk_widget_add_css_class (U.month_label, "title-3");
  adw_header_bar_set_title_widget (ADW_HEADER_BAR (header), U.month_label);

  /* left: weekday names over the month grid */
  GtkWidget *left = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_hexpand (left, TRUE);
  gtk_widget_set_margin_start (left, 16);
  gtk_widget_set_margin_end (left, 8);
  gtk_widget_set_margin_top (left, 8);
  gtk_widget_set_margin_bottom (left, 16);
  U.weekdays = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_append (GTK_BOX (left), U.weekdays);
  U.grid = gtk_grid_new ();
  gtk_grid_set_row_homogeneous (GTK_GRID (U.grid), TRUE);
  gtk_grid_set_column_homogeneous (GTK_GRID (U.grid), TRUE);
  gtk_grid_set_row_spacing (GTK_GRID (U.grid), 4);
  gtk_grid_set_column_spacing (GTK_GRID (U.grid), 4);
  gtk_widget_set_vexpand (U.grid, TRUE);
  gtk_box_append (GTK_BOX (left), U.grid);

  /* right: the chosen day */
  GtkWidget *right = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_size_request (right, 320, -1);
  gtk_widget_set_margin_start (right, 8);
  gtk_widget_set_margin_end (right, 16);
  gtk_widget_set_margin_top (right, 8);
  gtk_widget_set_margin_bottom (right, 16);
  U.agenda_title = gtk_label_new ("");
  gtk_widget_add_css_class (U.agenda_title, "title-2");
  gtk_label_set_xalign (GTK_LABEL (U.agenda_title), 0);
  gtk_label_set_wrap (GTK_LABEL (U.agenda_title), TRUE);
  gtk_box_append (GTK_BOX (right), U.agenda_title);
  U.agenda_list = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (U.agenda_list), GTK_SELECTION_NONE);
  gtk_widget_add_css_class (U.agenda_list, "boxed-list");
  GtkWidget *scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), U.agenda_list);
  gtk_widget_set_vexpand (scroll, TRUE);
  gtk_box_append (GTK_BOX (right), scroll);
  U.agenda_empty = gtk_label_new (TR ("No hay nada este día.", "Nothing on this day."));
  gtk_widget_add_css_class (U.agenda_empty, "dim-label");
  gtk_label_set_xalign (GTK_LABEL (U.agenda_empty), 0);
  gtk_box_append (GTK_BOX (right), U.agenda_empty);
  GtkWidget *add = gtk_button_new_with_label (TR ("Nuevo evento", "New event"));
  gtk_widget_add_css_class (add, "suggested-action");
  gtk_widget_add_css_class (add, "pill");
  g_signal_connect (add, "clicked", G_CALLBACK (on_new_event), NULL);
  gtk_box_append (GTK_BOX (right), add);

  GtkWidget *body = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_append (GTK_BOX (body), left);
  gtk_box_append (GTK_BOX (body), gtk_separator_new (GTK_ORIENTATION_VERTICAL));
  gtk_box_append (GTK_BOX (body), right);

  GtkWidget *tv = adw_toolbar_view_new ();
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (tv), header);
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (tv), body);

  U.view = tv;
  calendar_ui_refresh ();
  return tv;
}

void
calendar_ui_open (GtkWidget *parent)
{
  if (U.dialog)
    return;
  U.dialog = TRACK (adw_dialog_new ());
  adw_dialog_set_title (U.dialog, TR ("Calendario", "Calendar"));
  adw_dialog_set_content_width (U.dialog, 1080);
  adw_dialog_set_content_height (U.dialog, 720);
  g_signal_connect (U.dialog, "closed", G_CALLBACK (on_closed), NULL);
  adw_dialog_set_child (U.dialog, calendar_ui_view_new ());
  adw_dialog_present (U.dialog, parent);
}
