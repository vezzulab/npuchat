#include "calendar-ui.h"

#include <adwaita.h>
#include <string.h>

#include "calendar-alerts.h"
#include "calendar-grid.h"
#include "calendar-quick.h"
#include "calendar.h"
#include "i18n.h"
#include "selftest.h"

#define CHIPS_PER_DAY 3
#define GUTTER_PX 54

typedef enum { VIEW_DAY, VIEW_WEEK, VIEW_MONTH } View;

static struct {
  AdwDialog *dialog;     /* only when shown as a dialog */
  GtkWidget *view;       /* the calendar itself, wherever it lives */
  View       mode;
  gint64     anchor;     /* local midnight of the day everything is centred on */

  GtkWidget *title;
  GtkWidget *toggle[3];
  GtkWidget *stack;
  /* month */
  GtkWidget *month_weekdays, *month_grid;
  /* week and day */
  GtkWidget *week_head, *week_allday, *time_grid, *scroller;
  /* sidebar */
  GtkWidget *mini_title, *mini_grid, *cal_list;
  /* quick entry and search */
  GtkWidget *quick, *quick_preview;
  GtkWidget *search, *search_list, *search_empty;
  gboolean   scrolled;
} U;

static void free_data (gpointer data, GClosure *closure) { (void) closure; g_free (data); }

static void refresh_all (void);
static void open_editor (const CalEvent *existing, gint64 occ_start, gint64 start, gint64 end);

/* ---- names and dates ---------------------------------------------------- */

static const char *months_es[] = { "enero", "febrero", "marzo", "abril", "mayo", "junio", "julio", "agosto",
                                   "septiembre", "octubre", "noviembre", "diciembre" };
static const char *months_en[] = { "January", "February", "March", "April", "May", "June", "July", "August",
                                   "September", "October", "November", "December" };
static const char *months_short_es[] = { "ene", "feb", "mar", "abr", "may", "jun", "jul", "ago", "sep", "oct", "nov", "dic" };
static const char *months_short_en[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
/* index 0 = Monday */
static const char *days_es[] = { "lun", "mar", "mié", "jue", "vie", "sáb", "dom" };
static const char *days_en[] = { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };
static const char *days_long_es[] = { "lunes", "martes", "miércoles", "jueves", "viernes", "sábado", "domingo" };
static const char *days_long_en[] = { "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday" };

static gboolean
english (void)
{
  return i18n_lang () == LANG_EN;
}

/* Spanish weeks start on Monday, English ones on Sunday. 0 is Monday, 6 is Sunday. */
static int
week_start (void)
{
  return english () ? 6 : 0;
}

static GDateTime *
local_dt (gint64 t)
{
  return g_date_time_new_from_unix_local (t);
}

static gint64
now_unix (void)
{
  return g_get_real_time () / G_USEC_PER_SEC;
}

static gint64
shift_days (gint64 t, int n)
{
  g_autoptr (GDateTime) d = local_dt (t);
  g_autoptr (GDateTime) r = g_date_time_add_days (d, n);
  return g_date_time_to_unix (r);
}

static gint64
shift_months (gint64 t, int n)
{
  g_autoptr (GDateTime) d = local_dt (t);
  g_autoptr (GDateTime) r = g_date_time_add_months (d, n);
  return g_date_time_to_unix (r);
}

static gint64
month_start (gint64 t)
{
  g_autoptr (GDateTime) d = local_dt (t);
  g_autoptr (GDateTime) first = g_date_time_new_local (g_date_time_get_year (d), g_date_time_get_month (d), 1, 0, 0, 0);
  return g_date_time_to_unix (first);
}

/* the first day of the week that holds t */
static gint64
week_first (gint64 t)
{
  g_autoptr (GDateTime) d = local_dt (t);
  int back = (g_date_time_get_day_of_week (d) - 1 - week_start () + 7) % 7;
  return shift_days (calendar_day_start (t), -back);
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
  int dow = g_date_time_get_day_of_week (d) - 1, day = g_date_time_get_day_of_month (d), month = g_date_time_get_month (d);
  if (english ())
    return g_strdup_printf ("%s, %s %d", days_long_en[dow], months_en[month - 1], day);
  return g_strdup_printf ("%s %d de %s", days_long_es[dow], day, months_es[month - 1]);
}

static char *
format_short_day (gint64 t)
{
  g_autoptr (GDateTime) d = local_dt (t);
  int dow = g_date_time_get_day_of_week (d) - 1;
  return g_strdup_printf ("%s %d %s", english () ? days_en[dow] : days_es[dow], g_date_time_get_day_of_month (d),
                          (english () ? months_short_en : months_short_es)[g_date_time_get_month (d) - 1]);
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

static char *
time_text (const CalOccurrence *o)
{
  if (o->event->all_day)
    return g_strdup (TR ("Todo el día", "All day"));
  g_autofree char *a = format_hm (o->start);
  g_autofree char *b = format_hm (o->end);
  return g_strdup_printf ("%s – %s", a, b);
}

static const CalCalendar *
calendar_of (const CalEvent *ev)
{
  return calendar_calendar_find (calendar_default (), ev->calendar);
}

static void
add_color_class (GtkWidget *w, guint color)
{
  g_autofree char *cls = g_strdup_printf ("cal-c%u", color);
  gtk_widget_add_css_class (w, cls);
}

static void
add_class_n (GtkWidget *w, const char *prefix, guint n)
{
  g_autofree char *cls = g_strdup_printf ("%s%u", prefix, n);
  gtk_widget_add_css_class (w, cls);
}

static GtkWidget *
clear_box (GtkWidget *box)
{
  GtkWidget *kid;
  while ((kid = gtk_widget_get_first_child (box)))
    gtk_box_remove (GTK_BOX (box), kid);
  return box;
}

/* ---- repeating events: ask which showings a change applies to ----------- */

typedef void (*ScopeDone) (CalScope scope, gpointer data);

typedef struct {
  ScopeDone done;
  gpointer  data;
  GDestroyNotify free_data;
} ScopeAsk;

static void
on_scope_response (AdwAlertDialog *dialog, const char *response, gpointer data)
{
  (void) dialog;
  ScopeAsk *ask = data;
  gboolean chosen = TRUE;
  CalScope scope = CAL_SCOPE_ALL;
  if (g_str_equal (response, "one"))
    scope = CAL_SCOPE_ONE;
  else if (g_str_equal (response, "future"))
    scope = CAL_SCOPE_FUTURE;
  else if (!g_str_equal (response, "all"))
    chosen = FALSE;
  if (chosen)
    ask->done (scope, ask->data);
  if (ask->free_data)
    ask->free_data (ask->data);
  g_free (ask);
}

/* Takes ownership of data (freed after done runs, or when the question is dismissed). */
static void
ask_scope (const char *heading, ScopeDone done, gpointer data, GDestroyNotify free_data)
{
  ScopeAsk *ask = g_new0 (ScopeAsk, 1);
  ask->done = done;
  ask->data = data;
  ask->free_data = free_data;
  AdwDialog *d = adw_alert_dialog_new (heading, TR ("Este evento se repite.", "This event repeats."));
  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (d), "cancel", TR ("Cancelar", "Cancel"));
  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (d), "one", TR ("Solo este evento", "Only this event"));
  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (d), "future", TR ("Este y los siguientes", "This and following events"));
  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (d), "all", TR ("Todos los eventos", "All events"));
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (d), "one");
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (d), "cancel");
  g_signal_connect (d, "response", G_CALLBACK (on_scope_response), ask);
  adw_dialog_present (d, U.view);
}

typedef struct {
  char  *id;
  gint64 occ_start;
  CalEvent *edited;      /* for edits */
  AdwDialog *close_after; /* the editor, closed once the change is made */
} Change;

static void
change_free (gpointer p)
{
  Change *c = p;
  g_free (c->id);
  calendar_event_free (c->edited);
  g_free (c);
}

static void
do_delete (CalScope scope, gpointer data)
{
  Change *c = data;
  calendar_apply_delete (calendar_default (), c->id, c->occ_start, scope);
  if (c->close_after)
    adw_dialog_close (c->close_after);
  refresh_all ();
}

/* Deletes a showing, asking first which ones when the event repeats. */
static void
delete_showing (const char *id, gint64 occ_start, AdwDialog *close_after)
{
  const CalEvent *ev = calendar_find (calendar_default (), id);
  if (!ev)
    return;
  Change *c = g_new0 (Change, 1);
  c->id = g_strdup (id);
  c->occ_start = occ_start;
  c->close_after = close_after;
  if (ev->repeat == CAL_REPEAT_NONE)
    {
      do_delete (CAL_SCOPE_ALL, c);
      change_free (c);
    }
  else
    ask_scope (TR ("¿Borrar cuáles?", "Delete which ones?"), do_delete, c, change_free);
}

static void
do_edit (CalScope scope, gpointer data)
{
  Change *c = data;
  CalEvent *edited = g_steal_pointer (&c->edited);
  calendar_apply_edit (calendar_default (), c->id, c->occ_start, scope, edited);
  if (c->close_after)
    adw_dialog_close (c->close_after);
  refresh_all ();
}

/* Writes new values for a showing, asking first which ones when the event repeats.
 * Takes ownership of edited. */
static void
edit_showing (const char *id, gint64 occ_start, CalEvent *edited, AdwDialog *close_after)
{
  const CalEvent *ev = calendar_find (calendar_default (), id);
  if (!ev)
    {
      calendar_event_free (edited);
      return;
    }
  Change *c = g_new0 (Change, 1);
  c->id = g_strdup (id);
  c->occ_start = occ_start;
  c->edited = edited;
  c->close_after = close_after;
  if (ev->repeat == CAL_REPEAT_NONE)
    {
      do_edit (CAL_SCOPE_ALL, c);
      change_free (c);
    }
  else
    ask_scope (TR ("¿Cambiar cuáles?", "Change which ones?"), do_edit, c, change_free);
}

/* ---- details popover ---------------------------------------------------- */

typedef struct {
  char  *id;
  gint64 occ_start;
} PopData;

static void
pop_data_free (gpointer p)
{
  PopData *d = p;
  g_free (d->id);
  g_free (d);
}

static gboolean
unparent_idle (gpointer w)
{
  gtk_widget_unparent (GTK_WIDGET (w));
  return G_SOURCE_REMOVE;
}

static void
on_popover_closed (GtkPopover *pop, gpointer data)
{
  (void) data;
  g_idle_add (unparent_idle, pop);
}

static void
on_pop_edit (GtkButton *b, gpointer data)
{
  (void) b;
  GtkPopover *pop = data;
  PopData *d = g_object_get_data (G_OBJECT (pop), "pop");
  const CalEvent *ev = calendar_find (calendar_default (), d->id);
  gtk_popover_popdown (pop);
  if (ev)
    open_editor (ev, d->occ_start, 0, 0);
}

static void
on_pop_delete (GtkButton *b, gpointer data)
{
  (void) b;
  GtkPopover *pop = data;
  PopData *d = g_object_get_data (G_OBJECT (pop), "pop");
  gtk_popover_popdown (pop);
  delete_showing (d->id, d->occ_start, NULL);
}

static void
show_event_popover (const char *id, gint64 occ_start, GtkWidget *parent, GdkRectangle *where)
{
  const CalEvent *ev = calendar_find (calendar_default (), id);
  if (!ev)
    return;
  GtkWidget *pop = gtk_popover_new ();
  PopData *d = g_new0 (PopData, 1);
  d->id = g_strdup (id);
  d->occ_start = occ_start;
  g_object_set_data_full (G_OBJECT (pop), "pop", d, pop_data_free);

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_start (box, 6);
  gtk_widget_set_margin_end (box, 6);
  gtk_widget_set_margin_top (box, 6);
  gtk_widget_set_margin_bottom (box, 6);
  gtk_widget_set_size_request (box, 240, -1);

  GtkWidget *head = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (bar, "cal-bar");
  add_color_class (bar, calendar_of (ev)->color);
  gtk_box_append (GTK_BOX (head), bar);
  GtkWidget *title = gtk_label_new (ev->title);
  gtk_widget_add_css_class (title, "title-4");
  gtk_label_set_xalign (GTK_LABEL (title), 0);
  gtk_label_set_wrap (GTK_LABEL (title), TRUE);
  gtk_widget_set_hexpand (title, TRUE);
  gtk_box_append (GTK_BOX (head), title);
  gtk_box_append (GTK_BOX (box), head);

  CalOccurrence o = { ev, occ_start, occ_start + (ev->end - ev->start) };
  g_autofree char *day = format_day (occ_start);
  g_autofree char *when = time_text (&o);
  const char *rep = repeat_label (ev->repeat);
  g_autofree char *line = rep ? g_strdup_printf ("%s\n%s · %s", day, when, rep) : g_strdup_printf ("%s\n%s", day, when);
  GtkWidget *l = gtk_label_new (line);
  gtk_label_set_xalign (GTK_LABEL (l), 0);
  gtk_box_append (GTK_BOX (box), l);

  g_autofree char *cal_line = g_strdup_printf ("%s%s%s", calendar_of (ev)->name, *ev->location ? " · " : "", ev->location);
  GtkWidget *cl = gtk_label_new (cal_line);
  gtk_widget_add_css_class (cl, "dim-label");
  gtk_label_set_xalign (GTK_LABEL (cl), 0);
  gtk_label_set_wrap (GTK_LABEL (cl), TRUE);
  gtk_box_append (GTK_BOX (box), cl);
  if (*ev->notes)
    {
      GtkWidget *nl = gtk_label_new (ev->notes);
      gtk_label_set_xalign (GTK_LABEL (nl), 0);
      gtk_label_set_wrap (GTK_LABEL (nl), TRUE);
      gtk_label_set_max_width_chars (GTK_LABEL (nl), 36);
      gtk_box_append (GTK_BOX (box), nl);
    }

  GtkWidget *actions = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_margin_top (actions, 6);
  GtkWidget *edit = gtk_button_new_with_label (TR ("Editar", "Edit"));
  gtk_widget_set_hexpand (edit, TRUE);
  g_signal_connect (edit, "clicked", G_CALLBACK (on_pop_edit), pop);
  GtkWidget *del = gtk_button_new_with_label (TR ("Borrar", "Delete"));
  gtk_widget_add_css_class (del, "destructive-action");
  g_signal_connect (del, "clicked", G_CALLBACK (on_pop_delete), pop);
  gtk_box_append (GTK_BOX (actions), edit);
  gtk_box_append (GTK_BOX (actions), del);
  gtk_box_append (GTK_BOX (box), actions);

  gtk_popover_set_child (GTK_POPOVER (pop), box);
  gtk_widget_set_parent (pop, parent);
  if (where)
    gtk_popover_set_pointing_to (GTK_POPOVER (pop), where);
  g_signal_connect (pop, "closed", G_CALLBACK (on_popover_closed), NULL);
  gtk_popover_popup (GTK_POPOVER (pop));
}

/* ---- event editor ------------------------------------------------------- */

static const int alert_minutes[] = { CAL_NO_ALERT, 0, 5, 10, 15, 30, 60, 1440 };

typedef struct {
  AdwDialog *dialog;
  char      *id;          /* NULL when creating */
  gint64     occ_start;   /* the showing being edited */
  gint64     day;         /* local midnight of the chosen day */
  gint64     until;
  GtkWidget *title, *location, *notes, *all_day, *date_button, *calendar, *times_row;
  GtkWidget *start_h, *start_m, *end_h, *end_m, *repeat, *cal_combo, *alert;
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
  g_free (ev->location);
  ev->location = g_strdup (gtk_editable_get_text (GTK_EDITABLE (e->location)));
  ev->repeat = (CalRepeat) adw_combo_row_get_selected (ADW_COMBO_ROW (e->repeat));
  ev->until = ev->repeat == CAL_REPEAT_NONE ? 0 : e->until;
  ev->alert = alert_minutes[adw_combo_row_get_selected (ADW_COMBO_ROW (e->alert))];
  GPtrArray *cals = calendar_calendars (calendar_default ());
  guint ci = adw_combo_row_get_selected (ADW_COMBO_ROW (e->cal_combo));
  g_free (ev->calendar);
  ev->calendar = g_strdup (((CalCalendar *) cals->pdata[MIN (ci, cals->len - 1)])->id);

  U.anchor = e->day;
  if (e->id)
    {
      edit_showing (e->id, e->occ_start, ev, e->dialog);
      return;
    }
  calendar_add (calendar_default (), ev);
  adw_dialog_close (e->dialog);
  refresh_all ();
}

static void
on_editor_delete (GtkButton *button, gpointer data)
{
  (void) button;
  Editor *e = data;
  delete_showing (e->id, e->occ_start, e->dialog);
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

/* existing: edit it. Otherwise a new event from start to end (0 = the chosen day, next hour). */
static void
open_editor (const CalEvent *existing, gint64 occ_start, gint64 start, gint64 end)
{
  Editor *e = g_new0 (Editor, 1);
  e->id = existing ? g_strdup (existing->id) : NULL;
  e->occ_start = existing ? (occ_start ? occ_start : existing->start) : 0;
  e->until = existing ? existing->until : 0;

  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  int start_h, start_m = 0, end_h, end_m = 0;
  gboolean all_day = existing && existing->all_day;
  if (existing)
    {
      e->day = calendar_day_start (e->occ_start);
      if (!existing->all_day)
        {
          g_autoptr (GDateTime) s = local_dt (e->occ_start);
          g_autoptr (GDateTime) en = local_dt (e->occ_start + (existing->end - existing->start));
          start_h = g_date_time_get_hour (s);
          start_m = g_date_time_get_minute (s);
          end_h = g_date_time_get_hour (en);
          end_m = g_date_time_get_minute (en);
        }
      else
        start_h = end_h = 9;
    }
  else if (start)
    {
      e->day = calendar_day_start (start);
      g_autoptr (GDateTime) s = local_dt (start);
      g_autoptr (GDateTime) en = local_dt (end > start ? end : start + 3600);
      start_h = g_date_time_get_hour (s);
      start_m = g_date_time_get_minute (s);
      end_h = g_date_time_get_hour (en);
      end_m = g_date_time_get_minute (en);
    }
  else
    {
      e->day = U.anchor;
      start_h = (g_date_time_get_hour (now) + 1) % 24;
      end_h = (start_h + 1) % 24;
    }

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

  e->location = adw_entry_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (e->location), TR ("Lugar", "Location"));
  if (existing)
    gtk_editable_set_text (GTK_EDITABLE (e->location), existing->location);
  adw_preferences_group_add (main, e->location);

  e->all_day = adw_switch_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (e->all_day), TR ("Todo el día", "All day"));
  adw_switch_row_set_active (ADW_SWITCH_ROW (e->all_day), all_day);
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

  e->times_row = adw_action_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (e->times_row), TR ("Hora", "Time"));
  GtkWidget *times = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_append (GTK_BOX (times), time_spins (&e->start_h, &e->start_m, start_h, start_m));
  gtk_box_append (GTK_BOX (times), gtk_label_new ("–"));
  gtk_box_append (GTK_BOX (times), time_spins (&e->end_h, &e->end_m, end_h, end_m));
  adw_action_row_add_suffix (ADW_ACTION_ROW (e->times_row), times);
  adw_preferences_group_add (main, e->times_row);
  gtk_widget_set_visible (e->times_row, !all_day);
  g_signal_connect (e->all_day, "notify::active", G_CALLBACK (on_editor_all_day), e);

  e->repeat = adw_combo_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (e->repeat), TR ("Repetir", "Repeat"));
  const char *repeat_items[] = { TR ("Nunca", "Never"), TR ("Cada día", "Every day"), TR ("Cada semana", "Every week"),
                                 TR ("Cada mes", "Every month"), TR ("Cada año", "Every year"), NULL };
  g_autoptr (GtkStringList) list = gtk_string_list_new (repeat_items);
  adw_combo_row_set_model (ADW_COMBO_ROW (e->repeat), G_LIST_MODEL (list));
  adw_combo_row_set_selected (ADW_COMBO_ROW (e->repeat), existing ? (guint) existing->repeat : 0);
  adw_preferences_group_add (main, e->repeat);

  e->alert = adw_combo_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (e->alert), TR ("Aviso", "Alert"));
  const char *alert_items[] = { TR ("Ninguno", "None"), TR ("A la hora del evento", "At time of event"),
                                TR ("5 minutos antes", "5 minutes before"), TR ("10 minutos antes", "10 minutes before"),
                                TR ("15 minutos antes", "15 minutes before"), TR ("30 minutos antes", "30 minutes before"),
                                TR ("1 hora antes", "1 hour before"), TR ("1 día antes", "1 day before"), NULL };
  g_autoptr (GtkStringList) alerts = gtk_string_list_new (alert_items);
  adw_combo_row_set_model (ADW_COMBO_ROW (e->alert), G_LIST_MODEL (alerts));
  guint alert_sel = 0;
  for (guint i = 0; existing && i < G_N_ELEMENTS (alert_minutes); i++)
    if (alert_minutes[i] == existing->alert)
      alert_sel = i;
  adw_combo_row_set_selected (ADW_COMBO_ROW (e->alert), alert_sel);
  adw_preferences_group_add (main, e->alert);

  e->cal_combo = adw_combo_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (e->cal_combo), TR ("Calendario", "Calendar"));
  GPtrArray *cals = calendar_calendars (calendar_default ());
  g_autoptr (GtkStringList) names = gtk_string_list_new (NULL);
  guint cal_sel = 0;
  for (guint i = 0; i < cals->len; i++)
    {
      const CalCalendar *c = cals->pdata[i];
      gtk_string_list_append (names, c->name);
      if (existing && g_str_equal (c->id, existing->calendar))
        cal_sel = i;
    }
  adw_combo_row_set_model (ADW_COMBO_ROW (e->cal_combo), G_LIST_MODEL (names));
  adw_combo_row_set_selected (ADW_COMBO_ROW (e->cal_combo), cal_sel);
  adw_preferences_group_add (main, e->cal_combo);

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
  adw_dialog_present (e->dialog, U.view);
  gtk_widget_grab_focus (e->title);
}

/* ---- chips: clicking one shows the event ------------------------------- */

static void
on_chip_pressed (GtkGestureClick *g, int n_press, double x, double y, gpointer data)
{
  (void) x;
  (void) y;
  GtkWidget *chip = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (g));
  const char *id = data;
  gint64 *occ = g_object_get_data (G_OBJECT (chip), "occ");
  gtk_gesture_set_state (GTK_GESTURE (g), GTK_EVENT_SEQUENCE_CLAIMED); /* the day under it must not react */
  if (n_press >= 2)
    {
      const CalEvent *ev = calendar_find (calendar_default (), id);
      if (ev)
        open_editor (ev, *occ, 0, 0);
      return;
    }
  GdkRectangle r = { 0, 0, gtk_widget_get_width (chip), gtk_widget_get_height (chip) };
  show_event_popover (id, *occ, chip, &r);
}

static GtkWidget *
make_chip (const CalOccurrence *o, gboolean show_time)
{
  g_autofree char *hm = (o->event->all_day || !show_time) ? NULL : format_hm (o->start);
  g_autofree char *text = hm ? g_strdup_printf ("%s %s", hm, o->event->title) : g_strdup (o->event->title);
  GtkWidget *chip = gtk_label_new (text);
  gtk_label_set_xalign (GTK_LABEL (chip), 0);
  gtk_label_set_ellipsize (GTK_LABEL (chip), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class (chip, "cal-chip");
  add_color_class (chip, calendar_of (o->event)->color);
  g_object_set_data_full (G_OBJECT (chip), "occ", g_memdup2 (&o->start, sizeof o->start), g_free);
  GtkGesture *click = gtk_gesture_click_new ();
  g_signal_connect_data (click, "pressed", G_CALLBACK (on_chip_pressed), g_strdup (o->event->id), free_data, 0);
  gtk_widget_add_controller (chip, GTK_EVENT_CONTROLLER (click));
  return chip;
}

static gboolean
occurrence_visible (const CalOccurrence *o)
{
  return calendar_of (o->event)->visible;
}

/* ---- month view --------------------------------------------------------- */

static void
on_day_clicked (GtkGestureClick *g, int n_press, double x, double y, gpointer data)
{
  (void) g; (void) x; (void) y;
  gint64 *day = data;
  U.anchor = *day;
  if (n_press >= 2)
    {
      open_editor (NULL, 0, *day + 9 * 3600, *day + 10 * 3600);
      return;
    }
  refresh_all ();
}

static void
rebuild_month (void)
{
  GtkWidget *kid;
  while ((kid = gtk_widget_get_first_child (U.month_grid)))
    gtk_grid_remove (GTK_GRID (U.month_grid), kid);

  gint64 m0 = month_start (U.anchor);
  g_autoptr (GDateTime) first = local_dt (m0);
  int offset = (g_date_time_get_day_of_week (first) - 1 - week_start () + 7) % 7;
  gint64 grid_start = shift_days (m0, -offset);
  GArray *occ = calendar_occurrences (calendar_default (), grid_start, shift_days (grid_start, 42));
  gint64 today = calendar_day_start (now_unix ());

  for (int i = 0; i < 42; i++)
    {
      gint64 day = shift_days (grid_start, i), next = calendar_day_next (day);
      g_autoptr (GDateTime) d = local_dt (day);

      GtkWidget *cell = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
      gtk_widget_add_css_class (cell, "cal-day");
      if (g_date_time_get_month (d) != g_date_time_get_month (first))
        gtk_widget_add_css_class (cell, "other");
      if (day == U.anchor)
        gtk_widget_add_css_class (cell, "selected");
      gtk_widget_set_hexpand (cell, TRUE);
      gtk_widget_set_vexpand (cell, TRUE);
      gtk_widget_set_overflow (cell, GTK_OVERFLOW_HIDDEN);

      g_autofree char *num = g_strdup_printf ("%d", g_date_time_get_day_of_month (d));
      GtkWidget *number = gtk_label_new (num);
      gtk_widget_add_css_class (number, "cal-num");
      if (day == today)
        gtk_widget_add_css_class (number, "today");
      gtk_widget_set_halign (number, GTK_ALIGN_START);
      gtk_box_append (GTK_BOX (cell), number);

      guint shown = 0, total = 0;
      for (guint k = 0; k < occ->len; k++)
        {
          const CalOccurrence *o = &g_array_index (occ, CalOccurrence, k);
          if (o->start >= next || o->end <= day || !occurrence_visible (o))
            continue;
          total++;
          if (shown < CHIPS_PER_DAY)
            {
              gtk_box_append (GTK_BOX (cell), make_chip (o, TRUE));
              shown++;
            }
        }
      if (total > shown)
        {
          g_autofree char *more = g_strdup_printf (TR ("+%u más", "+%u more"), total - shown);
          GtkWidget *l = gtk_label_new (more);
          gtk_widget_add_css_class (l, "cal-more");
          gtk_widget_set_halign (l, GTK_ALIGN_START);
          gtk_box_append (GTK_BOX (cell), l);
        }

      GtkGesture *click = gtk_gesture_click_new ();
      g_signal_connect_data (click, "pressed", G_CALLBACK (on_day_clicked), g_memdup2 (&day, sizeof day), free_data, 0);
      gtk_widget_add_controller (cell, GTK_EVENT_CONTROLLER (click));
      gtk_grid_attach (GTK_GRID (U.month_grid), cell, i % 7, i / 7, 1, 1);
    }
  g_array_free (occ, TRUE);

  clear_box (U.month_weekdays);
  for (int i = 0; i < 7; i++)
    {
      int dow = (week_start () + i) % 7;
      GtkWidget *l = gtk_label_new (english () ? days_en[dow] : days_es[dow]);
      gtk_widget_add_css_class (l, "cal-weekday");
      gtk_widget_set_hexpand (l, TRUE);
      gtk_box_append (GTK_BOX (U.month_weekdays), l);
    }
}

/* ---- week and day views ------------------------------------------------- */

static void
on_head_clicked (GtkGestureClick *g, int n, double x, double y, gpointer data)
{
  (void) g; (void) n; (void) x; (void) y;
  gint64 *day = data;
  U.anchor = *day;
  U.mode = VIEW_DAY;
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (U.toggle[VIEW_DAY]), TRUE);
}

/* the scroll range exists only after the first layout, so this waits for it */
static void
scroll_to_hour (gpointer data)
{
  if (!U.scroller)
    return;
  GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (U.scroller));
  gtk_adjustment_set_value (adj, cal_grid_hour_y (U.time_grid, GPOINTER_TO_INT (data)));
}

static void
rebuild_week (void)
{
  int n = U.mode == VIEW_DAY ? 1 : 7;
  gint64 first = U.mode == VIEW_DAY ? U.anchor : week_first (U.anchor);
  gint64 today = calendar_day_start (now_unix ());
  gint64 end = shift_days (first, n);

  clear_box (U.week_head);
  clear_box (U.week_allday);
  GtkWidget *spacer = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_size_request (spacer, GUTTER_PX, -1);
  gtk_box_append (GTK_BOX (U.week_head), spacer);
  GtkWidget *heads = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_set_homogeneous (GTK_BOX (heads), TRUE);
  gtk_widget_set_hexpand (heads, TRUE);
  gtk_box_append (GTK_BOX (U.week_head), heads);

  GtkWidget *spacer2 = gtk_label_new (TR ("todo el día", "all day"));
  gtk_widget_add_css_class (spacer2, "cal-weekday");
  gtk_widget_set_size_request (spacer2, GUTTER_PX, -1);
  gtk_widget_set_valign (spacer2, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (U.week_allday), spacer2);
  GtkWidget *strips = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_set_homogeneous (GTK_BOX (strips), TRUE);
  gtk_widget_set_hexpand (strips, TRUE);
  gtk_box_append (GTK_BOX (U.week_allday), strips);

  GArray *occ = calendar_occurrences (calendar_default (), first, end);
  gboolean any_allday = FALSE;
  for (int i = 0; i < n; i++)
    {
      gint64 day = shift_days (first, i), next = calendar_day_next (day);
      g_autoptr (GDateTime) d = local_dt (day);
      int dow = g_date_time_get_day_of_week (d) - 1;

      GtkWidget *head = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
      gtk_widget_set_halign (head, GTK_ALIGN_CENTER);
      GtkWidget *name = gtk_label_new (english () ? days_en[dow] : days_es[dow]);
      gtk_widget_add_css_class (name, "cal-weekday");
      g_autofree char *num = g_strdup_printf ("%d", g_date_time_get_day_of_month (d));
      GtkWidget *number = gtk_label_new (num);
      gtk_widget_add_css_class (number, "cal-num");
      gtk_widget_add_css_class (number, "big");
      if (day == today)
        gtk_widget_add_css_class (number, "today");
      gtk_box_append (GTK_BOX (head), name);
      gtk_box_append (GTK_BOX (head), number);
      if (U.mode == VIEW_WEEK)
        {
          GtkGesture *click = gtk_gesture_click_new ();
          g_signal_connect_data (click, "released", G_CALLBACK (on_head_clicked), g_memdup2 (&day, sizeof day), free_data, 0);
          gtk_widget_add_controller (head, GTK_EVENT_CONTROLLER (click));
        }
      gtk_box_append (GTK_BOX (heads), head);

      GtkWidget *strip = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
      gtk_widget_set_margin_start (strip, 2);
      gtk_widget_set_margin_end (strip, 2);
      for (guint k = 0; k < occ->len; k++)
        {
          const CalOccurrence *o = &g_array_index (occ, CalOccurrence, k);
          if (!o->event->all_day || o->start >= next || o->end <= day || !occurrence_visible (o))
            continue;
          gtk_box_append (GTK_BOX (strip), make_chip (o, FALSE));
          any_allday = TRUE;
        }
      gtk_box_append (GTK_BOX (strips), strip);
    }
  g_array_free (occ, TRUE);
  gtk_widget_set_visible (U.week_allday, any_allday);

  cal_grid_set_days (U.time_grid, first, n);
  if (!U.scrolled)
    {
      /* start the view near the morning, or an hour before now when looking at today */
      g_autoptr (GDateTime) nowd = g_date_time_new_now_local ();
      int hour = (U.anchor == today || (first <= today && today < end)) ? MAX (g_date_time_get_hour (nowd) - 1, 0) : 7;
      U.scrolled = TRUE;
      g_idle_add_once (scroll_to_hour, GINT_TO_POINTER (hour));
    }
}

static void
on_grid_picked (const char *id, gint64 occ_start, GtkWidget *grid, double x, double y, gpointer data)
{
  (void) data;
  GdkRectangle r = { (int) x, (int) y, 1, 1 };
  show_event_popover (id, occ_start, grid, &r);
}

static void
on_grid_open (const char *id, gint64 occ_start, gpointer data)
{
  (void) data;
  const CalEvent *ev = calendar_find (calendar_default (), id);
  if (ev)
    open_editor (ev, occ_start, 0, 0);
}

static gint64 test_create_start, test_create_end;

static void
on_grid_create (gint64 start, gint64 end, gpointer data)
{
  (void) data;
  test_create_start = start;
  test_create_end = end;
  open_editor (NULL, 0, start, end);
}

static void
on_grid_moved (const char *id, gint64 occ_start, gint64 new_start, gint64 new_end, gpointer data)
{
  (void) data;
  const CalEvent *ev = calendar_find (calendar_default (), id);
  if (!ev)
    return;
  CalEvent *edited = calendar_event_copy (ev);
  edited->start = new_start;
  edited->end = new_end;
  edit_showing (id, occ_start, edited, NULL);
}

/* ---- sidebar: mini month and calendars ---------------------------------- */

static void
on_mini_day (GtkButton *b, gpointer data)
{
  (void) data;
  gint64 *day = g_object_get_data (G_OBJECT (b), "day");
  U.anchor = *day;
  refresh_all ();
}

static void
rebuild_mini (void)
{
  GtkWidget *kid;
  while ((kid = gtk_widget_get_first_child (U.mini_grid)))
    gtk_grid_remove (GTK_GRID (U.mini_grid), kid);

  gint64 m0 = month_start (U.anchor);
  g_autoptr (GDateTime) first = local_dt (m0);
  g_autofree char *title = g_strdup_printf ("%s %d", (english () ? months_en : months_es)[g_date_time_get_month (first) - 1],
                                            g_date_time_get_year (first));
  title[0] = (char) g_ascii_toupper (title[0]);
  gtk_label_set_text (GTK_LABEL (U.mini_title), title);

  for (int i = 0; i < 7; i++)
    {
      int dow = (week_start () + i) % 7;
      g_autofree char *letter = g_strndup (english () ? days_en[dow] : days_es[dow], 1);
      letter[0] = (char) g_ascii_toupper (letter[0]);
      GtkWidget *l = gtk_label_new (letter);
      gtk_widget_add_css_class (l, "cal-weekday");
      gtk_grid_attach (GTK_GRID (U.mini_grid), l, i, 0, 1, 1);
    }
  int offset = (g_date_time_get_day_of_week (first) - 1 - week_start () + 7) % 7;
  gint64 start = shift_days (m0, -offset);
  gint64 today = calendar_day_start (now_unix ());
  gint64 sel_first = U.mode == VIEW_WEEK ? week_first (U.anchor) : U.anchor;
  gint64 sel_end = U.mode == VIEW_WEEK ? shift_days (sel_first, 7) : U.mode == VIEW_DAY ? calendar_day_next (U.anchor) : 0;
  for (int i = 0; i < 42; i++)
    {
      gint64 day = shift_days (start, i);
      g_autoptr (GDateTime) d = local_dt (day);
      g_autofree char *num = g_strdup_printf ("%d", g_date_time_get_day_of_month (d));
      GtkWidget *b = gtk_button_new_with_label (num);
      gtk_widget_add_css_class (b, "flat");
      gtk_widget_add_css_class (b, "mini-day");
      if (g_date_time_get_month (d) != g_date_time_get_month (first))
        gtk_widget_add_css_class (b, "other");
      if (day == today)
        gtk_widget_add_css_class (b, "today");
      if (sel_end && day >= sel_first && day < sel_end)
        gtk_widget_add_css_class (b, "selected");
      g_object_set_data_full (G_OBJECT (b), "day", g_memdup2 (&day, sizeof day), g_free);
      g_signal_connect (b, "clicked", G_CALLBACK (on_mini_day), NULL);
      gtk_grid_attach (GTK_GRID (U.mini_grid), b, i % 7, 1 + i / 7, 1, 1);
    }
}

static void
on_mini_prev (GtkButton *b, gpointer d)
{
  (void) b; (void) d;
  U.anchor = shift_months (U.anchor, -1);
  refresh_all ();
}

static void
on_mini_next (GtkButton *b, gpointer d)
{
  (void) b; (void) d;
  U.anchor = shift_months (U.anchor, 1);
  refresh_all ();
}

static void
on_cal_visible (GtkCheckButton *check, gpointer data)
{
  CalCalendar *c = calendar_calendar_find (calendar_default (), data);
  c->visible = gtk_check_button_get_active (check);
  calendar_calendar_changed (calendar_default ());
  /* the sidebar already shows the new state; only the events need redrawing */
  gtk_widget_queue_draw (U.time_grid);
  rebuild_month ();
  rebuild_week ();
}

static void
on_cal_rename (GtkEditable *entry, gpointer data)
{
  CalCalendar *c = calendar_calendar_find (calendar_default (), data);
  g_free (c->name);
  c->name = g_strdup (gtk_editable_get_text (entry));
  calendar_calendar_changed (calendar_default ());
}

static void
on_cal_color (GtkButton *b, gpointer data)
{
  CalCalendar *c = calendar_calendar_find (calendar_default (), data);
  c->color = (guint) GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (b), "color"));
  calendar_calendar_changed (calendar_default ());
  refresh_all ();
}

static void
on_cal_remove_response (AdwAlertDialog *dialog, const char *response, gpointer data)
{
  (void) dialog;
  if (g_str_equal (response, "delete"))
    {
      calendar_calendar_remove (calendar_default (), data);
      refresh_all ();
    }
}

static void
on_cal_remove (GtkButton *b, gpointer data)
{
  (void) b;
  const CalCalendar *c = calendar_calendar_find (calendar_default (), data);
  if (calendar_calendars (calendar_default ())->len < 2)
    return;
  g_autofree char *body = g_strdup_printf (TR ("Se borrarán también todos los eventos de «%s».", "All the events in “%s” will be deleted too."), c->name);
  AdwDialog *ask = adw_alert_dialog_new (TR ("¿Borrar este calendario?", "Delete this calendar?"), body);
  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (ask), "cancel", TR ("Cancelar", "Cancel"));
  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (ask), "delete", TR ("Borrar", "Delete"));
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (ask), "delete", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (ask), "cancel");
  g_signal_connect_data (ask, "response", G_CALLBACK (on_cal_remove_response), g_strdup (c->id), free_data, 0);
  adw_dialog_present (ask, U.view);
}

static GtkWidget *
calendar_settings_popover (const CalCalendar *c)
{
  GtkWidget *pop = gtk_popover_new ();
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_start (box, 6);
  gtk_widget_set_margin_end (box, 6);
  gtk_widget_set_margin_top (box, 6);
  gtk_widget_set_margin_bottom (box, 6);
  GtkWidget *entry = gtk_entry_new ();
  gtk_editable_set_text (GTK_EDITABLE (entry), c->name);
  g_signal_connect_data (entry, "changed", G_CALLBACK (on_cal_rename), g_strdup (c->id), free_data, 0);
  gtk_box_append (GTK_BOX (box), entry);

  GtkWidget *swatches = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (swatches), 6);
  gtk_grid_set_column_spacing (GTK_GRID (swatches), 6);
  for (guint i = 0; i < CAL_N_COLORS; i++)
    {
      GtkWidget *s = gtk_button_new ();
      gtk_widget_add_css_class (s, "swatch");
      gtk_widget_add_css_class (s, "circular");
      add_class_n (s, "sw-", i);
      if (i == c->color)
        gtk_widget_add_css_class (s, "current");
      g_object_set_data (G_OBJECT (s), "color", GUINT_TO_POINTER (i));
      g_signal_connect_data (s, "clicked", G_CALLBACK (on_cal_color), g_strdup (c->id), free_data, 0);
      gtk_grid_attach (GTK_GRID (swatches), s, (int) (i % 4), (int) (i / 4), 1, 1);
    }
  gtk_box_append (GTK_BOX (box), swatches);

  if (calendar_calendars (calendar_default ())->len > 1)
    {
      GtkWidget *del = gtk_button_new_with_label (TR ("Borrar calendario", "Delete calendar"));
      gtk_widget_add_css_class (del, "destructive-action");
      g_signal_connect_data (del, "clicked", G_CALLBACK (on_cal_remove), g_strdup (c->id), free_data, 0);
      gtk_box_append (GTK_BOX (box), del);
    }
  gtk_popover_set_child (GTK_POPOVER (pop), box);
  return pop;
}

static void
on_new_calendar (GtkButton *b, gpointer d)
{
  (void) b; (void) d;
  GPtrArray *cals = calendar_calendars (calendar_default ());
  calendar_calendar_add (calendar_default (), TR ("Nuevo calendario", "New calendar"), cals->len);
  refresh_all ();
}

static void
rebuild_calendars (void)
{
  clear_box (U.cal_list);
  GPtrArray *cals = calendar_calendars (calendar_default ());
  for (guint i = 0; i < cals->len; i++)
    {
      const CalCalendar *c = cals->pdata[i];
      GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
      GtkWidget *check = gtk_check_button_new ();
      gtk_widget_add_css_class (check, "cal-check");
      add_class_n (check, "ck-", c->color);
      gtk_check_button_set_active (GTK_CHECK_BUTTON (check), c->visible);
      g_signal_connect_data (check, "toggled", G_CALLBACK (on_cal_visible), g_strdup (c->id), free_data, 0);
      gtk_box_append (GTK_BOX (row), check);
      GtkWidget *name = gtk_label_new (c->name);
      gtk_label_set_xalign (GTK_LABEL (name), 0);
      gtk_label_set_ellipsize (GTK_LABEL (name), PANGO_ELLIPSIZE_END);
      gtk_widget_set_hexpand (name, TRUE);
      gtk_box_append (GTK_BOX (row), name);
      GtkWidget *more = gtk_menu_button_new ();
      gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (more), "view-more-symbolic");
      gtk_widget_add_css_class (more, "flat");
      gtk_widget_add_css_class (more, "circular");
      gtk_menu_button_set_popover (GTK_MENU_BUTTON (more), calendar_settings_popover (c));
      gtk_box_append (GTK_BOX (row), more);
      gtk_box_append (GTK_BOX (U.cal_list), row);
    }
}

/* ---- search ------------------------------------------------------------- */

static char *
fold (const char *text)
{
  g_autofree char *ascii = g_str_to_ascii (text, NULL);
  return g_ascii_strdown (ascii, -1);
}

static void
on_result_activated (AdwActionRow *row, gpointer data)
{
  (void) data;
  gint64 *start = g_object_get_data (G_OBJECT (row), "start");
  U.anchor = calendar_day_start (*start);
  gtk_editable_set_text (GTK_EDITABLE (U.search), "");
  refresh_all ();
}

static void
on_search_changed (GtkSearchEntry *entry, gpointer data)
{
  (void) data;
  const char *text = gtk_editable_get_text (GTK_EDITABLE (entry));
  if (!*text)
    {
      refresh_all ();
      return;
    }
  g_autofree char *needle = fold (text);
  gtk_list_box_remove_all (GTK_LIST_BOX (U.search_list));
  gint64 today = calendar_day_start (now_unix ());
  GArray *occ = calendar_occurrences (calendar_default (), shift_days (today, -365), shift_days (today, 730));
  guint shown = 0;
  for (guint i = 0; i < occ->len && shown < 200; i++)
    {
      const CalOccurrence *o = &g_array_index (occ, CalOccurrence, i);
      g_autofree char *hay = fold (o->event->title);
      g_autofree char *loc = fold (o->event->location);
      g_autofree char *notes = fold (o->event->notes);
      if (!strstr (hay, needle) && !strstr (loc, needle) && !strstr (notes, needle))
        continue;
      GtkWidget *row = adw_action_row_new ();
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), o->event->title);
      g_autofree char *day = format_day (o->start);
      g_autofree char *when = time_text (o);
      g_autofree char *sub = g_strdup_printf ("%s · %s", day, when);
      adw_action_row_set_subtitle (ADW_ACTION_ROW (row), sub);
      GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
      gtk_widget_add_css_class (bar, "cal-bar");
      add_color_class (bar, calendar_of (o->event)->color);
      adw_action_row_add_prefix (ADW_ACTION_ROW (row), bar);
      gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), TRUE);
      g_object_set_data_full (G_OBJECT (row), "start", g_memdup2 (&o->start, sizeof o->start), g_free);
      g_signal_connect (row, "activated", G_CALLBACK (on_result_activated), NULL);
      gtk_list_box_append (GTK_LIST_BOX (U.search_list), row);
      shown++;
    }
  g_array_free (occ, TRUE);
  gtk_widget_set_visible (U.search_empty, shown == 0);
  gtk_stack_set_visible_child_name (GTK_STACK (U.stack), "search");
}

/* ---- header: title and navigation --------------------------------------- */

static char *
title_text (void)
{
  g_autoptr (GDateTime) d = local_dt (U.anchor);
  int month = g_date_time_get_month (d), year = g_date_time_get_year (d);
  if (U.mode == VIEW_MONTH)
    {
      char *s = g_strdup_printf ("%s %d", (english () ? months_en : months_es)[month - 1], year);
      s[0] = (char) g_ascii_toupper (s[0]);
      return s;
    }
  if (U.mode == VIEW_DAY)
    {
      g_autofree char *day = format_day (U.anchor);
      char *s = g_strdup_printf ("%s %d", day, year);
      s[0] = (char) g_ascii_toupper (s[0]);
      return s;
    }
  gint64 a = week_first (U.anchor), b = shift_days (a, 6);
  g_autoptr (GDateTime) da = local_dt (a);
  g_autoptr (GDateTime) db = local_dt (b);
  const char **ms = english () ? months_short_en : months_short_es;
  if (g_date_time_get_month (da) == g_date_time_get_month (db))
    return g_strdup_printf ("%d – %d %s %d", g_date_time_get_day_of_month (da), g_date_time_get_day_of_month (db),
                            ms[g_date_time_get_month (db) - 1], g_date_time_get_year (db));
  return g_strdup_printf ("%d %s – %d %s %d", g_date_time_get_day_of_month (da), ms[g_date_time_get_month (da) - 1],
                          g_date_time_get_day_of_month (db), ms[g_date_time_get_month (db) - 1], g_date_time_get_year (db));
}

static void
refresh_all (void)
{
  calendar_alerts_reschedule (); /* every change goes through here */
  if (!U.view)
    return;
  g_autofree char *t = title_text ();
  gtk_label_set_text (GTK_LABEL (U.title), t);
  rebuild_mini ();
  rebuild_calendars ();
  if (U.mode == VIEW_MONTH)
    {
      gtk_stack_set_visible_child_name (GTK_STACK (U.stack), "month");
      rebuild_month ();
    }
  else
    {
      gtk_stack_set_visible_child_name (GTK_STACK (U.stack), "week");
      rebuild_week ();
    }
}

void
calendar_ui_refresh (void)
{
  refresh_all ();
}

static void
on_step (GtkButton *b, gpointer data)
{
  (void) b;
  int dir = GPOINTER_TO_INT (data);
  U.anchor = U.mode == VIEW_MONTH ? shift_months (month_start (U.anchor), dir) : shift_days (U.anchor, dir * (U.mode == VIEW_WEEK ? 7 : 1));
  refresh_all ();
}

static void
on_today (GtkButton *b, gpointer d)
{
  (void) b; (void) d;
  U.anchor = calendar_day_start (now_unix ());
  U.scrolled = FALSE;
  refresh_all ();
}

static void
on_mode_toggled (GtkToggleButton *b, gpointer data)
{
  if (!gtk_toggle_button_get_active (b))
    return;
  U.mode = (View) GPOINTER_TO_INT (data);
  refresh_all ();
}

static void
on_new_event (GtkButton *b, gpointer d)
{
  (void) b; (void) d;
  open_editor (NULL, 0, 0, 0);
}

/* ---- quick entry -------------------------------------------------------- */

static gboolean
quick_month_first (void)
{
  return english ();
}

static char *
quick_summary (const CalQuick *q)
{
  g_autofree char *day = format_short_day (q->start);
  g_autofree char *a = format_hm (q->start);
  g_autofree char *b = format_hm (q->end);
  const char *rep = repeat_label (q->repeat);
  g_autofree char *when = q->all_day ? g_strdup (TR ("todo el día", "all day")) : g_strdup_printf ("%s – %s", a, b);
  const char *title = *q->title ? q->title : TR ("Sin título", "Untitled");
  return rep ? g_strdup_printf ("%s  ·  %s  ·  %s  ·  %s", title, day, when, rep)
             : g_strdup_printf ("%s  ·  %s  ·  %s", title, day, when);
}

static void
on_quick_changed (GtkEditable *entry, gpointer data)
{
  (void) data;
  const char *text = gtk_editable_get_text (entry);
  CalQuick q;
  if (!*text || !calendar_quick_parse (text, now_unix (), quick_month_first (), &q))
    {
      gtk_widget_set_visible (U.quick_preview, FALSE);
      return;
    }
  g_autofree char *summary = quick_summary (&q);
  gtk_label_set_text (GTK_LABEL (U.quick_preview), summary);
  gtk_widget_set_visible (U.quick_preview, TRUE);
  calendar_quick_clear (&q);
}

static void
on_quick_activate (GtkEntry *entry, gpointer data)
{
  (void) data;
  const char *text = gtk_editable_get_text (GTK_EDITABLE (entry));
  CalQuick q;
  if (!calendar_quick_parse (text, now_unix (), quick_month_first (), &q))
    return;
  CalEvent *ev = calendar_event_new (*q.title ? q.title : TR ("Sin título", "Untitled"), q.start, q.end, q.all_day);
  ev->repeat = q.repeat;
  calendar_add (calendar_default (), ev);
  U.anchor = calendar_day_start (q.start);
  calendar_quick_clear (&q);
  gtk_editable_set_text (GTK_EDITABLE (entry), "");
  refresh_all ();
}

/* ---- window ------------------------------------------------------------- */

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

static GtkWidget *
mode_button (const char *label, View v, GtkToggleButton *group)
{
  GtkWidget *b = gtk_toggle_button_new_with_label (label);
  if (group)
    gtk_toggle_button_set_group (GTK_TOGGLE_BUTTON (b), group);
  g_signal_connect (b, "toggled", G_CALLBACK (on_mode_toggled), GINT_TO_POINTER (v));
  return b;
}

static GtkWidget *
build_sidebar (void)
{
  GtkWidget *side = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_size_request (side, 232, -1);
  gtk_widget_set_hexpand (side, FALSE); /* its labels ask to expand; the calendar should get the room */
  gtk_widget_add_css_class (side, "cal-sidebar");
  gtk_widget_set_margin_start (side, 14);
  gtk_widget_set_margin_end (side, 10);
  gtk_widget_set_margin_top (side, 8);
  gtk_widget_set_margin_bottom (side, 14);

  GtkWidget *mini_head = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 2);
  U.mini_title = gtk_label_new ("");
  gtk_widget_add_css_class (U.mini_title, "heading");
  gtk_label_set_xalign (GTK_LABEL (U.mini_title), 0);
  gtk_widget_set_hexpand (U.mini_title, TRUE);
  gtk_box_append (GTK_BOX (mini_head), U.mini_title);
  GtkWidget *prev = gtk_button_new_from_icon_name ("go-previous-symbolic");
  GtkWidget *next = gtk_button_new_from_icon_name ("go-next-symbolic");
  gtk_widget_add_css_class (prev, "flat");
  gtk_widget_add_css_class (next, "flat");
  g_signal_connect (prev, "clicked", G_CALLBACK (on_mini_prev), NULL);
  g_signal_connect (next, "clicked", G_CALLBACK (on_mini_next), NULL);
  gtk_box_append (GTK_BOX (mini_head), prev);
  gtk_box_append (GTK_BOX (mini_head), next);
  gtk_box_append (GTK_BOX (side), mini_head);

  U.mini_grid = gtk_grid_new ();
  gtk_grid_set_column_homogeneous (GTK_GRID (U.mini_grid), TRUE);
  gtk_box_append (GTK_BOX (side), U.mini_grid);

  GtkWidget *cal_head = gtk_label_new (TR ("Calendarios", "Calendars"));
  gtk_widget_add_css_class (cal_head, "heading");
  gtk_label_set_xalign (GTK_LABEL (cal_head), 0);
  gtk_widget_set_margin_top (cal_head, 10);
  gtk_box_append (GTK_BOX (side), cal_head);
  U.cal_list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  gtk_box_append (GTK_BOX (side), U.cal_list);
  GtkWidget *add = gtk_button_new_with_label (TR ("Nuevo calendario", "New calendar"));
  gtk_widget_add_css_class (add, "flat");
  gtk_widget_set_halign (add, GTK_ALIGN_START);
  g_signal_connect (add, "clicked", G_CALLBACK (on_new_calendar), NULL);
  gtk_box_append (GTK_BOX (side), add);
  return side;
}

static gboolean
on_key (GtkEventControllerKey *c, guint keyval, guint keycode, GdkModifierType state, gpointer data)
{
  (void) c; (void) keycode; (void) data;
  if (!(state & GDK_CONTROL_MASK))
    return FALSE;
  switch (keyval)
    {
    case GDK_KEY_n:     open_editor (NULL, 0, 0, 0); return TRUE;
    case GDK_KEY_f:     gtk_widget_grab_focus (U.search); return TRUE;
    case GDK_KEY_t:     on_today (NULL, NULL); return TRUE;
    case GDK_KEY_l:     gtk_widget_grab_focus (U.quick); return TRUE;
    case GDK_KEY_1:     gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (U.toggle[VIEW_DAY]), TRUE); return TRUE;
    case GDK_KEY_2:     gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (U.toggle[VIEW_WEEK]), TRUE); return TRUE;
    case GDK_KEY_3:     gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (U.toggle[VIEW_MONTH]), TRUE); return TRUE;
    case GDK_KEY_Left:  on_step (NULL, GINT_TO_POINTER (-1)); return TRUE;
    case GDK_KEY_Right: on_step (NULL, GINT_TO_POINTER (1)); return TRUE;
    default:            return FALSE;
    }
}

GtkWidget *
calendar_ui_view_new (void)
{
  U.mode = VIEW_WEEK;
  const char *want = g_getenv ("CALENDAR_VIEW");
  if (want && g_str_equal (want, "month"))
    U.mode = VIEW_MONTH;
  else if (want && g_str_equal (want, "day"))
    U.mode = VIEW_DAY;
  U.anchor = calendar_day_start (now_unix ());
  U.scrolled = FALSE;
  const char *theme = g_getenv ("CALENDAR_THEME");
  if (theme)
    adw_style_manager_set_color_scheme (adw_style_manager_get_default (),
                                        g_str_equal (theme, "light") ? ADW_COLOR_SCHEME_FORCE_LIGHT : ADW_COLOR_SCHEME_FORCE_DARK);

  GtkWidget *header = adw_header_bar_new ();
  GtkWidget *nav = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (nav, "linked");
  GtkWidget *prev = gtk_button_new_from_icon_name ("go-previous-symbolic");
  GtkWidget *next = gtk_button_new_from_icon_name ("go-next-symbolic");
  gtk_widget_set_tooltip_text (prev, TR ("Anterior", "Previous"));
  gtk_widget_set_tooltip_text (next, TR ("Siguiente", "Next"));
  g_signal_connect (prev, "clicked", G_CALLBACK (on_step), GINT_TO_POINTER (-1));
  g_signal_connect (next, "clicked", G_CALLBACK (on_step), GINT_TO_POINTER (1));
  gtk_box_append (GTK_BOX (nav), prev);
  gtk_box_append (GTK_BOX (nav), next);
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), nav);
  GtkWidget *today = gtk_button_new_with_label (TR ("Hoy", "Today"));
  g_signal_connect (today, "clicked", G_CALLBACK (on_today), NULL);
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), today);

  U.title = gtk_label_new ("");
  gtk_widget_add_css_class (U.title, "title-3");
  adw_header_bar_set_title_widget (ADW_HEADER_BAR (header), U.title);

  GtkWidget *modes = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (modes, "linked");
  U.toggle[VIEW_DAY] = mode_button (TR ("Día", "Day"), VIEW_DAY, NULL);
  U.toggle[VIEW_WEEK] = mode_button (TR ("Semana", "Week"), VIEW_WEEK, GTK_TOGGLE_BUTTON (U.toggle[VIEW_DAY]));
  U.toggle[VIEW_MONTH] = mode_button (TR ("Mes", "Month"), VIEW_MONTH, GTK_TOGGLE_BUTTON (U.toggle[VIEW_DAY]));
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (U.toggle[U.mode]), TRUE);
  for (int i = 0; i < 3; i++)
    gtk_box_append (GTK_BOX (modes), U.toggle[i]);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), modes);
  U.search = gtk_search_entry_new ();
  gtk_widget_set_size_request (U.search, 180, -1);
  gtk_search_entry_set_placeholder_text (GTK_SEARCH_ENTRY (U.search), TR ("Buscar eventos", "Search events"));
  g_signal_connect (U.search, "search-changed", G_CALLBACK (on_search_changed), NULL);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), U.search);
  GtkWidget *plus = gtk_button_new_from_icon_name ("list-add-symbolic");
  gtk_widget_set_tooltip_text (plus, TR ("Nuevo evento", "New event"));
  g_signal_connect (plus, "clicked", G_CALLBACK (on_new_event), NULL);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), plus);

  /* quick entry */
  U.quick = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (U.quick),
                                  TR ("Escribe un evento: «cena con Ana jueves 7pm»", "Type an event: “dinner with Ana thursday 7pm”"));
  gtk_entry_set_icon_from_icon_name (GTK_ENTRY (U.quick), GTK_ENTRY_ICON_PRIMARY, "list-add-symbolic");
  gtk_widget_add_css_class (U.quick, "cal-quick");
  g_signal_connect (U.quick, "changed", G_CALLBACK (on_quick_changed), NULL);
  g_signal_connect (U.quick, "activate", G_CALLBACK (on_quick_activate), NULL);
  U.quick_preview = gtk_label_new ("");
  gtk_widget_add_css_class (U.quick_preview, "cal-preview");
  gtk_label_set_xalign (GTK_LABEL (U.quick_preview), 0);
  gtk_widget_set_visible (U.quick_preview, FALSE);
  GtkWidget *quick_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_margin_start (quick_box, 12);
  gtk_widget_set_margin_end (quick_box, 16);
  gtk_widget_set_margin_top (quick_box, 8);
  gtk_widget_set_margin_bottom (quick_box, 8);
  gtk_box_append (GTK_BOX (quick_box), U.quick);
  gtk_box_append (GTK_BOX (quick_box), U.quick_preview);

  /* month page */
  GtkWidget *month = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_start (month, 12);
  gtk_widget_set_margin_end (month, 16);
  gtk_widget_set_margin_bottom (month, 16);
  U.month_weekdays = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_set_homogeneous (GTK_BOX (U.month_weekdays), TRUE);
  gtk_box_append (GTK_BOX (month), U.month_weekdays);
  U.month_grid = gtk_grid_new ();
  gtk_grid_set_row_homogeneous (GTK_GRID (U.month_grid), TRUE);
  gtk_grid_set_column_homogeneous (GTK_GRID (U.month_grid), TRUE);
  gtk_grid_set_row_spacing (GTK_GRID (U.month_grid), 4);
  gtk_grid_set_column_spacing (GTK_GRID (U.month_grid), 4);
  gtk_widget_set_vexpand (U.month_grid, TRUE);
  gtk_box_append (GTK_BOX (month), U.month_grid);

  /* week and day page */
  CalGridCallbacks cb = { on_grid_picked, on_grid_open, on_grid_create, on_grid_moved };
  U.time_grid = cal_grid_new (&cb, NULL);
  U.scroller = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (U.scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (U.scroller), U.time_grid);
  gtk_widget_set_vexpand (U.scroller, TRUE);
  U.week_head = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  U.week_allday = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (U.week_allday, "cal-allday");
  GtkWidget *week = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_margin_end (week, 8);
  gtk_box_append (GTK_BOX (week), U.week_head);
  gtk_box_append (GTK_BOX (week), U.week_allday);
  gtk_box_append (GTK_BOX (week), U.scroller);

  U.search_list = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (U.search_list), GTK_SELECTION_NONE);
  gtk_widget_add_css_class (U.search_list, "boxed-list");
  U.search_empty = gtk_label_new (TR ("Sin resultados.", "No results."));
  gtk_widget_add_css_class (U.search_empty, "dim-label");
  GtkWidget *results = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start (results, 12);
  gtk_widget_set_margin_end (results, 16);
  gtk_widget_set_margin_bottom (results, 16);
  gtk_box_append (GTK_BOX (results), U.search_list);
  gtk_box_append (GTK_BOX (results), U.search_empty);
  GtkWidget *results_scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (results_scroll), results);

  U.stack = gtk_stack_new ();
  gtk_stack_add_named (GTK_STACK (U.stack), results_scroll, "search");
  gtk_stack_add_named (GTK_STACK (U.stack), week, "week");
  gtk_stack_add_named (GTK_STACK (U.stack), month, "month");
  gtk_widget_set_hexpand (U.stack, TRUE);
  gtk_widget_set_vexpand (U.stack, TRUE);

  GtkWidget *main_col = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_hexpand (main_col, TRUE);
  gtk_box_append (GTK_BOX (main_col), quick_box);
  gtk_box_append (GTK_BOX (main_col), U.stack);

  GtkWidget *body = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_append (GTK_BOX (body), build_sidebar ());
  gtk_box_append (GTK_BOX (body), gtk_separator_new (GTK_ORIENTATION_VERTICAL));
  gtk_box_append (GTK_BOX (body), main_col);

  GtkWidget *tv = adw_toolbar_view_new ();
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (tv), header);
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (tv), body);
  GtkEventController *keys = gtk_event_controller_key_new ();
  gtk_event_controller_set_propagation_phase (keys, GTK_PHASE_CAPTURE);
  g_signal_connect (keys, "key-pressed", G_CALLBACK (on_key), NULL);
  gtk_widget_add_controller (tv, keys);
  U.view = tv;
  refresh_all ();
  return tv;
}

void
calendar_ui_open (GtkWidget *parent)
{
  if (U.dialog)
    return;
  AdwDialog *dialog = TRACK (adw_dialog_new ());
  adw_dialog_set_title (dialog, TR ("Calendario", "Calendar"));
  adw_dialog_set_content_width (dialog, 1180);
  adw_dialog_set_content_height (dialog, 780);
  g_signal_connect (dialog, "closed", G_CALLBACK (on_closed), NULL);
  GtkWidget *view = calendar_ui_view_new ();
  U.dialog = dialog;
  adw_dialog_set_child (dialog, view);
  adw_dialog_present (dialog, parent);
}

/* ---- self-test: drives the real grid the way a mouse would --------------- */

static gboolean
selftest_step (gpointer data)
{
  gboolean *result = data;
  Calendar *cal = calendar_default ();
  gint64 today = calendar_day_start (now_unix ());
  gint64 t10 = today + 10 * 3600;
  gboolean ok = TRUE;
  double x, y, x1, y1;

#define CHECK(cond, what) do { if (!(cond)) { g_print ("SELFTEST FAIL: %s\n", what); ok = FALSE; } } while (0)

  const char *id = NULL;
  {
    CalEvent *ev = calendar_event_new ("Test", t10, t10 + 3600, FALSE);
    id = calendar_add (cal, ev);
  }
  g_autofree char *event_id = g_strdup (id);
  U.mode = VIEW_WEEK;
  U.anchor = today;
  refresh_all ();
  /* the blocks are laid out during drawing, so give the toolkit a few frames */
  for (int i = 0; i < 30; i++)
    g_main_context_iteration (NULL, FALSE);
  gtk_widget_queue_draw (U.time_grid);
  for (int i = 0; i < 30; i++)
    g_main_context_iteration (NULL, FALSE);

  /* move: from the middle of the event to four hours later */
  CHECK (cal_grid_point (U.time_grid, t10 + 1800, &x, &y), "event position");
  cal_grid_point (U.time_grid, t10 + 4 * 3600 + 1800, &x1, &y1);
  cal_grid_test_drag (U.time_grid, x, y, x1, y1);
  const CalEvent *ev = calendar_find (cal, event_id);
  CHECK (ev && ev->start == t10 + 4 * 3600, "move changes the start by four hours");
  CHECK (ev && ev->end - ev->start == 3600, "move keeps the duration");

  /* stretch: drag the bottom edge down one hour */
  for (int i = 0; i < 30; i++)
    g_main_context_iteration (NULL, FALSE);
  gint64 s = ev ? ev->start : t10;
  cal_grid_point (U.time_grid, s + 3600 - 300, &x, &y); /* inside the bottom strip */
  cal_grid_point (U.time_grid, s + 2 * 3600, &x1, &y1);
  cal_grid_test_drag (U.time_grid, x, y, x1, y1);
  ev = calendar_find (cal, event_id);
  CHECK (ev && ev->end == s + 2 * 3600, "stretching the bottom edge adds an hour");
  CHECK (ev && ev->start == s, "stretching keeps the start");

  /* a plain click must not move anything */
  for (int i = 0; i < 30; i++)
    g_main_context_iteration (NULL, FALSE);
  cal_grid_point (U.time_grid, s + 1800, &x, &y);
  cal_grid_test_drag (U.time_grid, x, y, x + 1, y + 1);
  ev = calendar_find (cal, event_id);
  CHECK (ev && ev->start == s, "a click without dragging leaves the event alone");

  /* create: drag across free time from 18:00 to 19:30 */
  cal_grid_point (U.time_grid, today + 18 * 3600, &x, &y);
  cal_grid_point (U.time_grid, today + 19 * 3600 + 1800, &x1, &y1);
  test_create_start = test_create_end = 0;
  cal_grid_test_drag (U.time_grid, x, y, x1, y1);
  CHECK (test_create_start == today + 18 * 3600 && test_create_end == today + 19 * 3600 + 1800, "dragging over free time proposes that range");
  g_print (ok ? "SELFTEST PASS\n" : "SELFTEST FAILED\n");
  *result = ok;
  return G_SOURCE_REMOVE;
}

gboolean
calendar_ui_selftest (void)
{
  static gboolean result = FALSE;
  selftest_step (&result);
  return result;
}
