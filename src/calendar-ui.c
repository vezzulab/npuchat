#include "calendar-ui.h"

#include <adwaita.h>
#include <string.h>

#include "calendar-alerts.h"
#include "calendar-accounts.h"
#include "calendar-caldav.h"
#include "calendar-grid.h"
#include "calendar-ics.h"
#include "calendar-print.h"
#include "calendar-subscribe.h"
#include "calendar-quick.h"
#include "calendar-settings.h"
#include "calendar.h"
#include "i18n.h"
#include "selftest.h"

#define CHIPS_PER_DAY 3
#define GUTTER_PX 58

typedef enum { VIEW_DAY, VIEW_WEEK, VIEW_MONTH, VIEW_YEAR } View;

static struct {
  AdwDialog *dialog;     /* only when shown as a dialog */
  GtkWidget *view;       /* the calendar itself, wherever it lives */
  View       mode;
  gint64     anchor;     /* local midnight of the day everything is centred on */

  GtkWidget *title;
  GtkWidget *toggle[4];
  GtkWidget *stack;
  /* month */
  GtkWidget *month_weekdays, *month_grid, *month_wn, *year_grid;
  /* week and day */
  GtkWidget *week_head, *week_allday, *time_grid, *scroller;
  /* sidebar */
  GtkWidget *mini_title, *mini_box, *cal_list, *sidebar, *new_button;
  /* quick entry and search */
  GtkWidget *quick, *quick_preview;
  GtkWidget *search, *search_list, *search_empty;
  gboolean   scrolled;
} U;

static char *sel_id;          /* the event picked last in the month view or a popover */
static gint64 sel_occ;
static CalEvent *clipboard_event;
static void (*background_handler) (gboolean background, gboolean autostart);
static gboolean standalone;   /* its own window, so it carries its own settings */
static char *editor_title_hint;  /* a title typed in quick entry, for the full editor */
static int editor_preset;        /* what a right-click menu asked the editor to start as */
#define PRESET_REMINDER 1
#define PRESET_ALLDAY 2

static void free_data (gpointer data, GClosure *closure) { (void) closure; g_free (data); }

static void refresh_all (void);
static void menu_for_event (GtkWidget *parent, double x, double y, const char *id, gint64 occ);
static void menu_for_time (GtkWidget *parent, double x, double y, gint64 time, gboolean with_time, gboolean offer_day);
static void print_view (void);
static void refresh_done (const char *calendar_id, guint events, const char *error, gpointer data);
static void open_editor (const CalEvent *existing, gint64 occ_start, gint64 start, gint64 end);

/* ---- appearance: system, light or dark (the standalone app keeps its own) -- */


static const char *
saved_theme (void)
{
  return calendar_settings ()->theme;
}
static void
apply_theme (const char *theme)
{
  AdwStyleManager *sm = adw_style_manager_get_default ();
  adw_style_manager_set_color_scheme (sm, g_str_equal (theme, "light") ? ADW_COLOR_SCHEME_FORCE_LIGHT
                                          : g_str_equal (theme, "dark") ? ADW_COLOR_SCHEME_FORCE_DARK
                                                                         : ADW_COLOR_SCHEME_DEFAULT);
}

void
calendar_ui_set_background_handler (void (*handler) (gboolean, gboolean))
{
  background_handler = handler;
}

void
calendar_ui_set_standalone (gboolean on)
{
  standalone = on;
}

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
  const CalSettings *cs = calendar_settings ();
  if (cs->days == 5)
    return 0;                        /* Monday to Friday always starts on Monday */
  if (cs->first_day)
    return cs->first_day - 1;
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

/* Popovers hang from the window itself, not from the chip or cell that was clicked: those are
 * rebuilt on every refresh, and a popover must not outlive its parent. */
static GtkWidget *
popover_home (GtkWidget *from, GdkRectangle *rect)
{
  graphene_point_t p = GRAPHENE_POINT_INIT ((float) rect->x, (float) rect->y);
  graphene_point_t out;
  if (U.view && gtk_widget_compute_point (from, U.view, &p, &out))
    {
      rect->x = (int) out.x;
      rect->y = (int) out.y;
      return U.view;
    }
  return from;
}

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
on_pop_done (GtkButton *b, gpointer data)
{
  (void) b;
  GtkPopover *pop = data;
  PopData *d = g_object_get_data (G_OBJECT (pop), "pop");
  const CalEvent *ev = calendar_find (calendar_default (), d->id);
  gtk_popover_popdown (pop);
  if (ev)
    calendar_set_done (calendar_default (), d->id, d->occ_start, !calendar_is_done (ev, d->occ_start));
  refresh_all ();
}

static void
show_event_popover (const char *id, gint64 occ_start, GtkWidget *parent, GdkRectangle *where)
{
  const CalEvent *ev = calendar_find (calendar_default (), id);
  if (!ev)
    return;
  g_free (sel_id);
  sel_id = g_strdup (id);
  sel_occ = occ_start;
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
  gtk_widget_set_size_request (box, 250, -1);

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
  g_autofree char *when_text = time_text (&o);
  if (ev->end - ev->start > 86400 || (!ev->all_day && calendar_day_start (o.end - 1) != calendar_day_start (occ_start)))
    {
      g_autofree char *last = format_day (o.end - 1);
      g_free (when_text);
      when_text = ev->all_day ? g_strdup_printf ("%s → %s", day, last) : g_strdup_printf ("→ %s", last);
    }
  const char *rep = repeat_label (ev->repeat);
  g_autofree char *line = rep ? g_strdup_printf ("%s\n%s · %s", day, when_text, rep) : g_strdup_printf ("%s\n%s", day, when_text);
  GtkWidget *l = gtk_label_new (line);
  gtk_label_set_xalign (GTK_LABEL (l), 0);
  gtk_box_append (GTK_BOX (box), l);

  g_autofree char *cal_line = g_strdup_printf ("%s%s%s", calendar_of (ev)->name, *ev->location ? " · " : "", ev->location);
  GtkWidget *cl = gtk_label_new (cal_line);
  gtk_widget_add_css_class (cl, "dim-label");
  gtk_label_set_xalign (GTK_LABEL (cl), 0);
  gtk_label_set_wrap (GTK_LABEL (cl), TRUE);
  gtk_box_append (GTK_BOX (box), cl);
  if (*ev->url)
    {
      g_autofree char *markup = g_markup_printf_escaped ("<a href=\"%s\">%s</a>", ev->url, ev->url);
      GtkWidget *ul = gtk_label_new (NULL);
      gtk_label_set_markup (GTK_LABEL (ul), markup);
      gtk_label_set_xalign (GTK_LABEL (ul), 0);
      gtk_label_set_ellipsize (GTK_LABEL (ul), PANGO_ELLIPSIZE_END);
      gtk_label_set_max_width_chars (GTK_LABEL (ul), 34);
      gtk_box_append (GTK_BOX (box), ul);
    }
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
  if (ev->reminder)
    {
      GtkWidget *done = gtk_button_new_with_label (calendar_is_done (ev, occ_start) ? TR ("Marcar pendiente", "Mark not done")
                                                                                    : TR ("Marcar como hecho", "Mark done"));
      gtk_widget_set_hexpand (done, TRUE);
      g_signal_connect (done, "clicked", G_CALLBACK (on_pop_done), pop);
      gtk_box_append (GTK_BOX (box), done);
    }
  if (!calendar_of (ev)->url)
    {
      GtkWidget *edit = gtk_button_new_with_label (TR ("Editar", "Edit"));
      gtk_widget_set_hexpand (edit, TRUE);
      g_signal_connect (edit, "clicked", G_CALLBACK (on_pop_edit), pop);
      GtkWidget *del = gtk_button_new_with_label (TR ("Borrar", "Delete"));
      gtk_widget_add_css_class (del, "destructive-action");
      g_signal_connect (del, "clicked", G_CALLBACK (on_pop_delete), pop);
      gtk_box_append (GTK_BOX (actions), edit);
      gtk_box_append (GTK_BOX (actions), del);
      gtk_box_append (GTK_BOX (box), actions);
    }

  gtk_popover_set_child (GTK_POPOVER (pop), box);
  GdkRectangle rect = where ? *where : (GdkRectangle) { 0, 0, gtk_widget_get_width (parent), gtk_widget_get_height (parent) };
  GtkWidget *home = popover_home (parent, &rect);
  gtk_widget_set_parent (pop, home);
  gtk_popover_set_pointing_to (GTK_POPOVER (pop), &rect);
  g_signal_connect (pop, "closed", G_CALLBACK (on_popover_closed), NULL);
  gtk_popover_popup (GTK_POPOVER (pop));
}
/* ---- event editor ------------------------------------------------------- */

static const int alert_minutes[] = { -1, 0, 5, 10, 15, 30, 60, 120, 1440, 2880, 10080 };

static GtkStringList *
alert_choices (void)
{
  const char *items[] = { TR ("Ninguno", "None"), TR ("A la hora del evento", "At time of event"),
                          TR ("5 minutos antes", "5 minutes before"), TR ("10 minutos antes", "10 minutes before"),
                          TR ("15 minutos antes", "15 minutes before"), TR ("30 minutos antes", "30 minutes before"),
                          TR ("1 hora antes", "1 hour before"), TR ("2 horas antes", "2 hours before"),
                          TR ("1 día antes", "1 day before"), TR ("2 días antes", "2 days before"),
                          TR ("1 semana antes", "1 week before"), NULL };
  return gtk_string_list_new (items);
}

/* "15 minutes before", "1 day 15 h before", "9 h after the start" */
static char *
alert_label (int minutes)
{
  if (minutes == -1)
    return g_strdup (TR ("Ninguno", "None"));
  if (minutes == 0)
    return g_strdup (TR ("A la hora del evento", "At time of event"));
  int m = minutes < 0 ? -minutes : minutes;
  int weeks = m / 10080, days = (m % 10080) / 1440, hours = (m % 1440) / 60, mins = m % 60;
  GString *parts = g_string_new (NULL);
  if (weeks)
    g_string_append_printf (parts, "%d %s", weeks, weeks == 1 ? TR ("semana", "week") : TR ("semanas", "weeks"));
  if (days)
    g_string_append_printf (parts, "%s%d %s", parts->len ? " " : "", days, days == 1 ? TR ("día", "day") : TR ("días", "days"));
  if (hours)
    g_string_append_printf (parts, "%s%d %s", parts->len ? " " : "", hours, hours == 1 && !english () ? "hora" : english () ? (hours == 1 ? "hour" : "hours") : "horas");
  if (mins)
    g_string_append_printf (parts, "%s%d min", parts->len ? " " : "", mins);
  char *label = minutes > 0 ? g_strdup_printf (TR ("%s antes", "%s before"), parts->str)
                            : g_strdup_printf (TR ("%s después del inicio", "%s after the start"), parts->str);
  g_string_free (parts, TRUE);
  return label;
}

static guint
alert_index (int minutes)
{
  for (guint i = 0; i < G_N_ELEMENTS (alert_minutes); i++)
    if (alert_minutes[i] == minutes)
      return i;
  return 0;
}

/* new events start with the alert and the calendar chosen in the settings */
static void
apply_defaults (CalEvent *ev)
{
  const CalSettings *cs = calendar_settings ();
  if (cs->default_alert >= 0 && calendar_event_alert_count (ev) == 0)
    calendar_event_add_alert (ev, cs->default_alert);
  const CalCalendar *c = cs->default_calendar ? calendar_calendar_find (calendar_default (), cs->default_calendar) : NULL;
  if (c && !c->url)
    {
      g_free (ev->calendar);
      ev->calendar = g_strdup (c->id);
    }
}

/* the calendars the user can add events to: not the subscriptions */
static GPtrArray *
own_calendars (void)
{
  GPtrArray *own = g_ptr_array_new ();
  GPtrArray *all = calendar_calendars (calendar_default ());
  for (guint i = 0; i < all->len; i++)
    if (!((CalCalendar *) all->pdata[i])->url)
      g_ptr_array_add (own, all->pdata[i]);
  return own;
}

typedef struct {
  AdwDialog *dialog;
  char      *id;          /* NULL when creating */
  gint64     occ_start;   /* the showing being edited */
  gint64     day, end_day, until_day;   /* local midnights */
  GtkWidget *title, *location, *url, *notes, *all_day, *reminder;
  GtkWidget *date_btn[3], *date_cal[3];                 /* start, end, repeat-until */
  GtkWidget *start_row, *end_date_row, *end_time_row;
  GtkWidget *start_h, *start_m, *end_h, *end_m;
  GtkWidget *repeat, *interval_row, *weekday_row, *monthly_row, *ends_row, *until_row, *count_row;
  GtkWidget *weekday[7];
  GtkWidget *alert1, *alert2, *cal_combo;
  GArray    *alert_values;   /* minutes for each choice: the usual ones, plus any the event already has */
  GArray    *extra_alerts;   /* alerts beyond the two rows, kept as they are */
  GPtrArray *own;
} Editor;

static Editor *last_editor;   /* for the self-test, which fills the form like a person would */

static void
editor_free (AdwDialog *dialog, gpointer data)
{
  (void) dialog;
  Editor *e = data;
  if (last_editor == e)
    last_editor = NULL;
  g_free (e->id);
  g_ptr_array_unref (e->own);
  g_array_free (e->alert_values, TRUE);
  g_array_free (e->extra_alerts, TRUE);
  g_free (e);
}

static gint64 *
editor_day (Editor *e, int which)
{
  return which == 0 ? &e->day : which == 1 ? &e->end_day : &e->until_day;
}

static void
editor_update_date_labels (Editor *e)
{
  for (int i = 0; i < 3; i++)
    {
      gint64 d = *editor_day (e, i);
      g_autofree char *text = format_day (d ? d : e->day);
      gtk_menu_button_set_label (GTK_MENU_BUTTON (e->date_btn[i]), text);
    }
}

/* shows only the rows that make sense for what is chosen */
static void
editor_sync (Editor *e)
{
  gboolean all_day = adw_switch_row_get_active (ADW_SWITCH_ROW (e->all_day));
  gboolean reminder = adw_switch_row_get_active (ADW_SWITCH_ROW (e->reminder));
  guint rep = adw_combo_row_get_selected (ADW_COMBO_ROW (e->repeat));
  guint ends = adw_combo_row_get_selected (ADW_COMBO_ROW (e->ends_row));
  gtk_widget_set_visible (e->start_row, !all_day);
  gtk_widget_set_visible (e->end_date_row, !reminder);
  gtk_widget_set_visible (e->end_time_row, !reminder && !all_day);
  gtk_widget_set_visible (e->interval_row, rep != 0);
  gtk_widget_set_visible (e->weekday_row, rep == CAL_REPEAT_WEEKLY);
  gtk_widget_set_visible (e->monthly_row, rep == CAL_REPEAT_MONTHLY);
  gtk_widget_set_visible (e->ends_row, rep != 0);
  gtk_widget_set_visible (e->until_row, rep != 0 && ends == 1);
  gtk_widget_set_visible (e->count_row, rep != 0 && ends == 2);
  static const char *units_es[] = { "", "días", "semanas", "meses", "años" };
  static const char *units_en[] = { "", "days", "weeks", "months", "years" };
  adw_action_row_set_subtitle (ADW_ACTION_ROW (e->interval_row), english () ? units_en[MIN (rep, 4)] : units_es[MIN (rep, 4)]);
}

static void
on_editor_changed (GObject *obj, GParamSpec *pspec, gpointer data)
{
  (void) obj; (void) pspec;
  editor_sync (data);
}

static void
on_editor_date (GtkCalendar *calendar, gpointer data)
{
  Editor *e = data;
  int which = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (calendar), "which"));
  g_autoptr (GDateTime) d = gtk_calendar_get_date (calendar);
  g_autoptr (GDateTime) m = g_date_time_new_local (g_date_time_get_year (d), g_date_time_get_month (d),
                                                    g_date_time_get_day_of_month (d), 0, 0, 0);
  gint64 picked = g_date_time_to_unix (m);
  gint64 *day = editor_day (e, which);
  if (which == 0)
    {
      /* moving the start moves the end with it, so the event keeps its length in days */
      gint64 delta = picked - e->day;
      e->end_day = calendar_day_start (e->end_day + delta + 12 * 3600);
    }
  *day = picked;
  if (e->end_day < e->day)
    e->end_day = e->day;
  editor_update_date_labels (e);
}

static gint64
editor_time (gint64 day, GtkWidget *h, GtkWidget *m)
{
  g_autoptr (GDateTime) d = local_dt (day);
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
  gboolean reminder = adw_switch_row_get_active (ADW_SWITCH_ROW (e->reminder));
  gint64 start = all_day ? e->day : editor_time (e->day, e->start_h, e->start_m);
  gint64 end;
  if (reminder)
    end = all_day ? calendar_day_next (e->day) : start + 900;
  else if (all_day)
    end = calendar_day_next (MAX (e->end_day, e->day));
  else
    end = editor_time (e->end_day, e->end_h, e->end_m);
  if (end <= start)
    end = start + 900;

  const char *title = gtk_editable_get_text (GTK_EDITABLE (e->title));
  CalEvent *ev = calendar_event_new (*title ? title : TR ("Sin título", "Untitled"), start, end, all_day);
  ev->reminder = reminder;
  g_free (ev->notes);
  ev->notes = g_strdup (gtk_editable_get_text (GTK_EDITABLE (e->notes)));
  g_free (ev->location);
  ev->location = g_strdup (gtk_editable_get_text (GTK_EDITABLE (e->location)));
  g_free (ev->url);
  ev->url = g_strdup (gtk_editable_get_text (GTK_EDITABLE (e->url)));

  guint rep = adw_combo_row_get_selected (ADW_COMBO_ROW (e->repeat));
  ev->repeat = (CalRepeat) rep;
  if (rep != 0)
    {
      guint step = (guint) adw_spin_row_get_value (ADW_SPIN_ROW (e->interval_row));
      ev->interval = step > 1 ? step : 0;
      if (rep == CAL_REPEAT_WEEKLY)
        {
          guint mask = 0;
          for (int i = 0; i < 7; i++)
            if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (e->weekday[i])))
              mask |= 1u << i;
          g_autoptr (GDateTime) sd = local_dt (start);
          /* only the start's own weekday is the same as not choosing any */
          ev->weekdays = mask == (1u << (g_date_time_get_day_of_week (sd) - 1)) ? 0 : mask;
        }
      if (rep == CAL_REPEAT_MONTHLY)
        ev->monthly = (CalMonthly) adw_combo_row_get_selected (ADW_COMBO_ROW (e->monthly_row));
      guint ends = adw_combo_row_get_selected (ADW_COMBO_ROW (e->ends_row));
      if (ends == 1)
        ev->until = calendar_day_start (MAX (e->until_day, e->day));
      else if (ends == 2)
        ev->count = (int) adw_spin_row_get_value (ADW_SPIN_ROW (e->count_row));
    }

  g_autoptr (GArray) chosen = g_array_new (FALSE, FALSE, sizeof (int));
  for (int k = 0; k < 2; k++)
    {
      guint sel = adw_combo_row_get_selected (ADW_COMBO_ROW (k ? e->alert2 : e->alert1));
      int m = sel < e->alert_values->len ? g_array_index (e->alert_values, int, sel) : -1;
      if (m != -1)
        g_array_append_val (chosen, m);
    }
  /* alerts past the second one are not shown, and not lost */
  g_array_append_vals (chosen, e->extra_alerts->data, e->extra_alerts->len);
  calendar_event_set_alerts (ev, (const int *) chosen->data, chosen->len);
  guint ci = adw_combo_row_get_selected (ADW_COMBO_ROW (e->cal_combo));
  g_free (ev->calendar);
  ev->calendar = g_strdup (((CalCalendar *) e->own->pdata[MIN (ci, e->own->len - 1)])->id);

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

/* two digits, so 5 reads 05 */
static gboolean
on_spin_output (GtkSpinButton *spin, gpointer data)
{
  (void) data;
  g_autofree char *text = g_strdup_printf ("%02d", (int) gtk_spin_button_get_value (spin));
  gtk_editable_set_text (GTK_EDITABLE (spin), text);
  return TRUE;
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
  g_signal_connect (*hour, "output", G_CALLBACK (on_spin_output), NULL);
  g_signal_connect (*minute, "output", G_CALLBACK (on_spin_output), NULL);
  gtk_box_append (GTK_BOX (box), *hour);
  gtk_box_append (GTK_BOX (box), gtk_label_new (":"));
  gtk_box_append (GTK_BOX (box), *minute);
  return box;
}

/* a row with a button that opens a calendar to pick the day */
static GtkWidget *
date_row (Editor *e, int which, const char *title)
{
  GtkWidget *row = adw_action_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
  e->date_btn[which] = gtk_menu_button_new ();
  gtk_widget_set_valign (e->date_btn[which], GTK_ALIGN_CENTER);
  e->date_cal[which] = gtk_calendar_new ();
  g_object_set_data (G_OBJECT (e->date_cal[which]), "which", GINT_TO_POINTER (which));
  GtkWidget *pop = gtk_popover_new ();
  gtk_popover_set_child (GTK_POPOVER (pop), e->date_cal[which]);
  gtk_menu_button_set_popover (GTK_MENU_BUTTON (e->date_btn[which]), pop);
  adw_action_row_add_suffix (ADW_ACTION_ROW (row), e->date_btn[which]);
  g_autoptr (GDateTime) d = local_dt (*editor_day (e, which) ? *editor_day (e, which) : e->day);
  /* gtk_calendar_set_date only exists from GTK 4.20; Ubuntu 24.04 ships 4.14 */
  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  gtk_calendar_select_day (GTK_CALENDAR (e->date_cal[which]), d);
  G_GNUC_END_IGNORE_DEPRECATIONS
  g_signal_connect (e->date_cal[which], "day-selected", G_CALLBACK (on_editor_date), e);
  return row;
}

/* existing: edit it. Otherwise a new event from start to end (0 = the chosen day, next hour). */
static void
open_editor (const CalEvent *existing, gint64 occ_start, gint64 start, gint64 end)
{
  if (existing && calendar_of (existing)->url)
    return;                          /* subscribed calendars are read-only */
  Editor *e = g_new0 (Editor, 1);
  e->own = own_calendars ();
  e->id = existing ? g_strdup (existing->id) : NULL;
  e->occ_start = existing ? (occ_start ? occ_start : existing->start) : 0;

  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  int start_h, start_m = 0, end_h, end_m = 0;
  gboolean all_day = existing && existing->all_day;
  gint64 span = existing ? existing->end - existing->start : 3600;
  if (existing)
    {
      e->day = calendar_day_start (e->occ_start);
      e->end_day = calendar_day_start (e->occ_start + span - (all_day ? 1 : 0));
      e->until_day = existing->until ? existing->until : e->day;
      g_autoptr (GDateTime) s = local_dt (e->occ_start);
      g_autoptr (GDateTime) en = local_dt (e->occ_start + span);
      start_h = g_date_time_get_hour (s);
      start_m = g_date_time_get_minute (s);
      end_h = g_date_time_get_hour (en);
      end_m = g_date_time_get_minute (en);
    }
  else if (start)
    {
      e->day = calendar_day_start (start);
      gint64 stop = end > start ? end : start + 3600;
      e->end_day = calendar_day_start (stop);
      e->until_day = e->day;
      g_autoptr (GDateTime) s = local_dt (start);
      g_autoptr (GDateTime) en = local_dt (stop);
      start_h = g_date_time_get_hour (s);
      start_m = g_date_time_get_minute (s);
      end_h = g_date_time_get_hour (en);
      end_m = g_date_time_get_minute (en);
    }
  else
    {
      e->day = e->end_day = e->until_day = U.anchor;
      start_h = (g_date_time_get_hour (now) + 1) % 24;
      end_h = (start_h + 1) % 24;
    }

  e->dialog = adw_dialog_new ();
  adw_dialog_set_title (e->dialog, existing ? TR ("Editar evento", "Edit event") : TR ("Nuevo evento", "New event"));
  adw_dialog_set_content_width (e->dialog, 480);
  adw_dialog_set_content_height (e->dialog, 720);
  g_signal_connect (e->dialog, "closed", G_CALLBACK (editor_free), e);

  GtkWidget *page = adw_preferences_page_new ();

  /* what */
  AdwPreferencesGroup *what = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  e->title = adw_entry_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (e->title), TR ("Título", "Title"));
  if (existing)
    gtk_editable_set_text (GTK_EDITABLE (e->title), existing->title);
  else if (editor_title_hint)
    gtk_editable_set_text (GTK_EDITABLE (e->title), editor_title_hint);
  g_clear_pointer (&editor_title_hint, g_free);
  adw_preferences_group_add (what, e->title);
  e->location = adw_entry_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (e->location), TR ("Lugar", "Location"));
  if (existing)
    gtk_editable_set_text (GTK_EDITABLE (e->location), existing->location);
  adw_preferences_group_add (what, e->location);
  e->url = adw_entry_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (e->url), TR ("Enlace", "Link"));
  if (existing)
    gtk_editable_set_text (GTK_EDITABLE (e->url), existing->url);
  adw_preferences_group_add (what, e->url);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), what);

  /* when */
  AdwPreferencesGroup *when = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  e->all_day = adw_switch_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (e->all_day), TR ("Todo el día", "All day"));
  adw_switch_row_set_active (ADW_SWITCH_ROW (e->all_day), all_day);
  adw_preferences_group_add (when, e->all_day);
  e->reminder = adw_switch_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (e->reminder), TR ("Es un recordatorio", "It is a reminder"));
  adw_action_row_set_subtitle (ADW_ACTION_ROW (e->reminder), TR ("Una tarea con casilla para marcar", "A task with a box to tick off"));
  adw_switch_row_set_active (ADW_SWITCH_ROW (e->reminder), existing && existing->reminder);
  adw_preferences_group_add (when, e->reminder);

  adw_preferences_group_add (when, date_row (e, 0, TR ("Empieza", "Starts")));
  e->start_row = adw_action_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (e->start_row), TR ("Hora de inicio", "Start time"));
  adw_action_row_add_suffix (ADW_ACTION_ROW (e->start_row), time_spins (&e->start_h, &e->start_m, start_h, start_m));
  adw_preferences_group_add (when, e->start_row);
  e->end_date_row = date_row (e, 1, TR ("Termina", "Ends"));
  adw_preferences_group_add (when, e->end_date_row);
  e->end_time_row = adw_action_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (e->end_time_row), TR ("Hora de fin", "End time"));
  adw_action_row_add_suffix (ADW_ACTION_ROW (e->end_time_row), time_spins (&e->end_h, &e->end_m, end_h, end_m));
  adw_preferences_group_add (when, e->end_time_row);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), when);

  /* repeat */
  AdwPreferencesGroup *rep = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  e->repeat = adw_combo_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (e->repeat), TR ("Repetir", "Repeat"));
  const char *repeat_items[] = { TR ("Nunca", "Never"), TR ("Cada día", "Every day"), TR ("Cada semana", "Every week"),
                                 TR ("Cada mes", "Every month"), TR ("Cada año", "Every year"), NULL };
  g_autoptr (GtkStringList) repeat_list = gtk_string_list_new (repeat_items);
  adw_combo_row_set_model (ADW_COMBO_ROW (e->repeat), G_LIST_MODEL (repeat_list));
  adw_combo_row_set_selected (ADW_COMBO_ROW (e->repeat), existing ? (guint) existing->repeat : 0);
  adw_preferences_group_add (rep, e->repeat);

  e->interval_row = adw_spin_row_new_with_range (1, 99, 1);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (e->interval_row), TR ("Cada", "Every"));
  adw_spin_row_set_value (ADW_SPIN_ROW (e->interval_row), existing && existing->interval ? existing->interval : 1);
  adw_preferences_group_add (rep, e->interval_row);

  e->weekday_row = adw_action_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (e->weekday_row), TR ("Los días", "On"));
  GtkWidget *wbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 2);
  gtk_widget_set_valign (wbox, GTK_ALIGN_CENTER);
  g_autoptr (GDateTime) sd = local_dt (existing ? e->occ_start : (start ? start : e->day));
  guint own_dow = (guint) (g_date_time_get_day_of_week (sd) - 1);
  for (int i = 0; i < 7; i++)
    {
      int dow = (week_start () + i) % 7;
      g_autofree char *letter = g_strndup (english () ? days_en[dow] : days_es[dow], 1);
      letter[0] = (char) g_ascii_toupper (letter[0]);
      GtkWidget *t = gtk_toggle_button_new_with_label (letter);
      gtk_widget_add_css_class (t, "circular");
      guint mask = existing && existing->weekdays ? existing->weekdays : (1u << own_dow);
      gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (t), (mask >> dow) & 1);
      e->weekday[dow] = t;
      gtk_box_append (GTK_BOX (wbox), t);
    }
  adw_action_row_add_suffix (ADW_ACTION_ROW (e->weekday_row), wbox);
  adw_preferences_group_add (rep, e->weekday_row);

  e->monthly_row = adw_combo_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (e->monthly_row), TR ("Cada mes el", "Each month on"));
  const char *monthly_items[] = { TR ("mismo día del mes", "the same date"), TR ("mismo día de la semana", "the same weekday"),
                                  TR ("último día del mes", "the last day"), NULL };
  g_autoptr (GtkStringList) monthly_list = gtk_string_list_new (monthly_items);
  adw_combo_row_set_model (ADW_COMBO_ROW (e->monthly_row), G_LIST_MODEL (monthly_list));
  adw_combo_row_set_selected (ADW_COMBO_ROW (e->monthly_row), existing ? (guint) existing->monthly : 0);
  adw_preferences_group_add (rep, e->monthly_row);

  e->ends_row = adw_combo_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (e->ends_row), TR ("Termina", "Ends"));
  const char *ends_items[] = { TR ("Nunca", "Never"), TR ("En una fecha", "On a date"), TR ("Después de varias veces", "After a number of times"), NULL };
  g_autoptr (GtkStringList) ends_list = gtk_string_list_new (ends_items);
  adw_combo_row_set_model (ADW_COMBO_ROW (e->ends_row), G_LIST_MODEL (ends_list));
  adw_combo_row_set_selected (ADW_COMBO_ROW (e->ends_row), existing ? (existing->until ? 1u : existing->count ? 2u : 0u) : 0);
  adw_preferences_group_add (rep, e->ends_row);
  e->until_row = date_row (e, 2, TR ("Hasta el", "Until"));
  adw_preferences_group_add (rep, e->until_row);
  e->count_row = adw_spin_row_new_with_range (1, 999, 1);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (e->count_row), TR ("Veces", "Times"));
  adw_spin_row_set_value (ADW_SPIN_ROW (e->count_row), existing && existing->count ? existing->count : 10);
  adw_preferences_group_add (rep, e->count_row);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), rep);

  /* alerts: the usual choices, plus whatever this event already has (iCloud's "15 hours before" for
   * all-day events, for one), so that saving never changes an alert it does not offer */
  e->alert_values = g_array_new (FALSE, FALSE, sizeof (int));
  e->extra_alerts = g_array_new (FALSE, FALSE, sizeof (int));
  for (guint i = 0; i < G_N_ELEMENTS (alert_minutes); i++)
    g_array_append_val (e->alert_values, alert_minutes[i]);
  for (guint i = 0; existing && i < calendar_event_alert_count (existing); i++)
    {
      int m = calendar_event_alert (existing, i);
      gboolean known = FALSE;
      for (guint k = 0; k < e->alert_values->len; k++)
        known |= g_array_index (e->alert_values, int, k) == m;
      if (!known)
        g_array_append_val (e->alert_values, m);
      if (i >= 2)
        g_array_append_val (e->extra_alerts, m);
    }
  AdwPreferencesGroup *alerts = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  for (int k = 0; k < 2; k++)
    {
      GtkWidget *row = adw_combo_row_new ();
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), k ? TR ("Segundo aviso", "Second alert") : TR ("Aviso", "Alert"));
      g_autoptr (GtkStringList) choices = gtk_string_list_new (NULL);
      for (guint i = 0; i < e->alert_values->len; i++)
        {
          g_autofree char *label = alert_label (g_array_index (e->alert_values, int, i));
          gtk_string_list_append (choices, label);
        }
      adw_combo_row_set_model (ADW_COMBO_ROW (row), G_LIST_MODEL (choices));
      int minutes = existing ? (k < (int) calendar_event_alert_count (existing) ? calendar_event_alert (existing, (guint) k) : -1)
                             : (k ? -1 : calendar_settings ()->default_alert);
      guint sel = 0;
      for (guint i = 0; i < e->alert_values->len; i++)
        if (g_array_index (e->alert_values, int, i) == minutes)
          sel = i;
      adw_combo_row_set_selected (ADW_COMBO_ROW (row), sel);
      adw_preferences_group_add (alerts, row);
      if (k)
        e->alert2 = row;
      else
        e->alert1 = row;
    }
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), alerts);

  /* more */
  AdwPreferencesGroup *more = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  e->cal_combo = adw_combo_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (e->cal_combo), TR ("Calendario", "Calendar"));
  g_autoptr (GtkStringList) names = gtk_string_list_new (NULL);
  guint cal_sel = 0;
  const char *want_cal = existing ? existing->calendar : calendar_settings ()->default_calendar;
  for (guint i = 0; i < e->own->len; i++)
    {
      const CalCalendar *c = e->own->pdata[i];
      gtk_string_list_append (names, c->name);
      if (want_cal && g_str_equal (c->id, want_cal))
        cal_sel = i;
    }
  adw_combo_row_set_model (ADW_COMBO_ROW (e->cal_combo), G_LIST_MODEL (names));
  adw_combo_row_set_selected (ADW_COMBO_ROW (e->cal_combo), cal_sel);
  adw_preferences_group_add (more, e->cal_combo);
  e->notes = adw_entry_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (e->notes), TR ("Notas", "Notes"));
  if (existing)
    gtk_editable_set_text (GTK_EDITABLE (e->notes), existing->notes);
  adw_preferences_group_add (more, e->notes);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), more);

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

  editor_update_date_labels (e);
  g_signal_connect (e->all_day, "notify::active", G_CALLBACK (on_editor_changed), e);
  g_signal_connect (e->reminder, "notify::active", G_CALLBACK (on_editor_changed), e);
  g_signal_connect (e->repeat, "notify::selected", G_CALLBACK (on_editor_changed), e);
  g_signal_connect (e->ends_row, "notify::selected", G_CALLBACK (on_editor_changed), e);
  if (!existing && editor_preset == PRESET_REMINDER)
    adw_switch_row_set_active (ADW_SWITCH_ROW (e->reminder), TRUE);
  else if (!existing && editor_preset == PRESET_ALLDAY)
    adw_switch_row_set_active (ADW_SWITCH_ROW (e->all_day), TRUE);
  editor_preset = 0;
  editor_sync (e);
  last_editor = e;
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
  g_free (sel_id);
  sel_id = g_strdup (id);
  sel_occ = *occ;
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

static void
on_chip_right (GtkGestureClick *g, int n_press, double x, double y, gpointer data)
{
  (void) n_press;
  GtkWidget *chip = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (g));
  gint64 *occ = g_object_get_data (G_OBJECT (chip), "occ");
  gtk_gesture_set_state (GTK_GESTURE (g), GTK_EVENT_SEQUENCE_CLAIMED);
  menu_for_event (chip, x, y, data, *occ);
}

static GtkWidget *
make_chip (const CalOccurrence *o, gboolean show_time)
{
  g_autofree char *hm = (o->event->all_day || o->event->reminder || !show_time) ? NULL : format_hm (o->start);
  gboolean done = o->event->reminder && calendar_is_done (o->event, o->start);
  const char *mark = o->event->reminder ? (done ? "● " : "○ ") : "";
  g_autofree char *text = hm ? g_strdup_printf ("%s %s", hm, o->event->title) : g_strdup_printf ("%s%s", mark, o->event->title);
  GtkWidget *chip = gtk_label_new (text);
  gtk_label_set_xalign (GTK_LABEL (chip), 0);
  gtk_label_set_ellipsize (GTK_LABEL (chip), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class (chip, "cal-chip");
  if (done)
    gtk_widget_add_css_class (chip, "done");
  add_color_class (chip, calendar_of (o->event)->color);
  g_object_set_data_full (G_OBJECT (chip), "occ", g_memdup2 (&o->start, sizeof o->start), g_free);
  GtkGesture *click = gtk_gesture_click_new ();
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click), GDK_BUTTON_PRIMARY);
  g_signal_connect_data (click, "pressed", G_CALLBACK (on_chip_pressed), g_strdup (o->event->id), free_data, 0);
  gtk_widget_add_controller (chip, GTK_EVENT_CONTROLLER (click));
  GtkGesture *right = gtk_gesture_click_new ();
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (right), GDK_BUTTON_SECONDARY);
  g_signal_connect_data (right, "pressed", G_CALLBACK (on_chip_right), g_strdup (o->event->id), free_data, 0);
  gtk_widget_add_controller (chip, GTK_EVENT_CONTROLLER (right));
  return chip;
}
static gboolean
occurrence_visible (const CalOccurrence *o)
{
  return calendar_of (o->event)->visible;
}

/* ---- right click: a small menu where you clicked ------------------------ */

typedef struct {
  gint64     time;        /* the moment or day that was clicked (0 for an event) */
  char      *id;          /* the event, when one was clicked */
  gint64     occ;
  GtkWidget *parent;      /* alive for as long as the menu is */
  double     x, y;
} Ctx;

typedef struct {
  const char *label;
  void      (*run) (const Ctx *c);
  gboolean    destructive;
} MenuEntry;

static void
ctx_free (gpointer p)
{
  Ctx *c = p;
  g_free (c->id);
  g_free (c);
}

static void
on_menu_item (GtkButton *b, gpointer data)
{
  GtkPopover *pop = data;
  const Ctx *c = g_object_get_data (G_OBJECT (pop), "ctx");
  void (*run) (const Ctx *) = g_object_get_data (G_OBJECT (b), "run");
  gtk_popover_popdown (pop);
  run (c);
}

static void
show_menu (Ctx *ctx, const MenuEntry *entries, guint n)
{
  GtkWidget *pop = gtk_popover_new ();
  g_object_set_data_full (G_OBJECT (pop), "ctx", ctx, ctx_free);
  gtk_widget_add_css_class (pop, "menu");
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_margin_start (box, 4);
  gtk_widget_set_margin_end (box, 4);
  gtk_widget_set_margin_top (box, 4);
  gtk_widget_set_margin_bottom (box, 4);
  for (guint i = 0; i < n; i++)
    {
      GtkWidget *b = gtk_button_new_with_label (entries[i].label);
      gtk_widget_add_css_class (b, "flat");
      if (entries[i].destructive)
        gtk_widget_add_css_class (b, "destructive-text");
      GtkWidget *label = gtk_button_get_child (GTK_BUTTON (b));
      if (GTK_IS_LABEL (label))
        gtk_label_set_xalign (GTK_LABEL (label), 0);
      g_object_set_data (G_OBJECT (b), "run", (gpointer) entries[i].run);
      g_signal_connect (b, "clicked", G_CALLBACK (on_menu_item), pop);
      gtk_box_append (GTK_BOX (box), b);
    }
  gtk_popover_set_child (GTK_POPOVER (pop), box);
  GdkRectangle where = { (int) ctx->x, (int) ctx->y, 1, 1 };
  gtk_widget_set_parent (pop, popover_home (ctx->parent, &where));
  gtk_popover_set_pointing_to (GTK_POPOVER (pop), &where);
  gtk_popover_set_has_arrow (GTK_POPOVER (pop), FALSE);
  gtk_popover_set_position (GTK_POPOVER (pop), GTK_POS_BOTTOM);
  g_signal_connect (pop, "closed", G_CALLBACK (on_popover_closed), NULL);
  gtk_popover_popup (GTK_POPOVER (pop));
}

static Ctx *
ctx_new (GtkWidget *parent, double x, double y, gint64 time, const char *id, gint64 occ)
{
  Ctx *c = g_new0 (Ctx, 1);
  c->parent = parent;
  c->x = x;
  c->y = y;
  c->time = time;
  c->id = g_strdup (id);
  c->occ = occ;
  return c;
}

static void
act_new_event (const Ctx *c)
{
  open_editor (NULL, 0, c->time, c->time + 3600);
}

static void
act_new_reminder (const Ctx *c)
{
  editor_preset = PRESET_REMINDER;
  open_editor (NULL, 0, c->time, c->time + 900);
}

static void
act_new_allday (const Ctx *c)
{
  editor_preset = PRESET_ALLDAY;
  open_editor (NULL, 0, calendar_day_start (c->time), 0);
}

static void
act_goto_day (const Ctx *c)
{
  U.anchor = calendar_day_start (c->time);
  U.mode = VIEW_DAY;
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (U.toggle[VIEW_DAY]), TRUE);
  refresh_all ();
}

static void
act_edit_event (const Ctx *c)
{
  const CalEvent *ev = calendar_find (calendar_default (), c->id);
  if (ev)
    open_editor (ev, c->occ, 0, 0);
}

static void
act_duplicate_event (const Ctx *c)
{
  const CalEvent *ev = calendar_find (calendar_default (), c->id);
  if (!ev)
    return;
  CalEvent *copy = calendar_event_copy (ev);
  g_clear_pointer (&copy->uid, g_free);
  g_clear_pointer (&copy->href, g_free);
  g_clear_pointer (&copy->etag, g_free);
  g_clear_pointer (&copy->exceptions, g_array_unref);
  g_clear_pointer (&copy->completed, g_array_unref);
  copy->recurrence_id = 0;
  copy->done = FALSE;
  /* the showing that was clicked, not the series' first one */
  gint64 length = ev->end - ev->start;
  copy->start = c->occ;
  copy->end = c->occ + length;
  if (calendar_of (ev)->url)
    {
      g_free (copy->calendar);
      copy->calendar = NULL;
    }
  calendar_add (calendar_default (), copy);
  refresh_all ();
}

static void
act_toggle_done (const Ctx *c)
{
  const CalEvent *ev = calendar_find (calendar_default (), c->id);
  if (ev)
    calendar_set_done (calendar_default (), c->id, c->occ, !calendar_is_done (ev, c->occ));
  refresh_all ();
}

static void
act_delete_event (const Ctx *c)
{
  delete_showing (c->id, c->occ, NULL);
}

static void
act_details (const Ctx *c)
{
  GdkRectangle r = { (int) c->x, (int) c->y, 1, 1 };
  show_event_popover (c->id, c->occ, c->parent, &r);
}

/* what to offer on an event */
static void
menu_for_event (GtkWidget *parent, double x, double y, const char *id, gint64 occ)
{
  const CalEvent *ev = calendar_find (calendar_default (), id);
  if (!ev)
    return;
  g_free (sel_id);
  sel_id = g_strdup (id);
  sel_occ = occ;
  Ctx *c = ctx_new (parent, x, y, 0, id, occ);
  if (calendar_of (ev)->url)
    {
      static const MenuEntry read_only[] = { { NULL, act_details, FALSE } };
      MenuEntry e[1] = { read_only[0] };
      e[0].label = TR ("Ver detalles", "Show details");
      show_menu (c, e, 1);
      return;
    }
  MenuEntry e[4];
  guint n = 0;
  e[n++] = (MenuEntry) { TR ("Editar…", "Edit…"), act_edit_event, FALSE };
  e[n++] = (MenuEntry) { TR ("Duplicar", "Duplicate"), act_duplicate_event, FALSE };
  if (ev->reminder)
    e[n++] = (MenuEntry) { calendar_is_done (ev, occ) ? TR ("Marcar pendiente", "Mark not done") : TR ("Marcar como hecho", "Mark done"),
                           act_toggle_done, FALSE };
  e[n++] = (MenuEntry) { TR ("Borrar", "Delete"), act_delete_event, TRUE };
  show_menu (c, e, n);
}

/* what to offer on a day or a moment: new things, or look at it closer */
static void
menu_for_time (GtkWidget *parent, double x, double y, gint64 time, gboolean with_time, gboolean offer_day)
{
  Ctx *c = ctx_new (parent, x, y, time, NULL, 0);
  g_autofree char *clock = NULL;
  if (with_time)
    {
      g_autoptr (GDateTime) d = g_date_time_new_from_unix_local (time);
      clock = g_date_time_format (d, "%H:%M");
    }
  g_autofree char *new_label = clock ? g_strdup_printf (TR ("Nuevo evento a las %s", "New event at %s"), clock) : g_strdup (TR ("Nuevo evento", "New event"));
  MenuEntry e[4];
  guint n = 0;
  e[n++] = (MenuEntry) { new_label, act_new_event, FALSE };
  e[n++] = (MenuEntry) { TR ("Nuevo recordatorio", "New reminder"), act_new_reminder, FALSE };
  e[n++] = (MenuEntry) { TR ("Evento de todo el día", "All-day event"), act_new_allday, FALSE };
  if (offer_day)
    e[n++] = (MenuEntry) { TR ("Ver este día", "Show this day"), act_goto_day, FALSE };
  show_menu (c, e, n);
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
on_day_right (GtkGestureClick *g, int n_press, double x, double y, gpointer data)
{
  (void) n_press;
  gint64 *day = data;
  GtkWidget *cell = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (g));
  U.anchor = *day;
  /* a new event in a month starts at nine, like a double click does */
  menu_for_time (cell, x, y, *day + 9 * 3600, FALSE, TRUE);
}

static void
rebuild_month (void)
{
  const CalSettings *cs = calendar_settings ();
  GtkWidget *kid;
  while ((kid = gtk_widget_get_first_child (U.month_grid)))
    gtk_grid_remove (GTK_GRID (U.month_grid), kid);
  clear_box (U.month_wn);

  gint64 m0 = month_start (U.anchor);
  g_autoptr (GDateTime) first = local_dt (m0);
  int offset = (g_date_time_get_day_of_week (first) - 1 - week_start () + 7) % 7;
  gint64 grid_start = shift_days (m0, -offset);
  GArray *occ = calendar_occurrences (calendar_default (), grid_start, shift_days (grid_start, 42));
  gint64 today = calendar_day_start (now_unix ());
  int ncols = cs->days;

  for (int i = 0; i < 42; i++)
    {
      gint64 day = shift_days (grid_start, i), next = calendar_day_next (day);
      g_autoptr (GDateTime) d = local_dt (day);
      int dow = g_date_time_get_day_of_week (d) - 1;        /* 0 Monday */
      int col = i % 7;
      if (ncols == 5)
        {
          if (dow >= 5)
            continue;                                       /* no weekend columns */
          col = dow;
        }
      if (col == 0 && cs->week_numbers)
        {
          g_autofree char *wn = g_strdup_printf ("%d", g_date_time_get_week_of_year (d));
          GtkWidget *l = gtk_label_new (wn);
          gtk_widget_add_css_class (l, "cal-weeknum");
          gtk_widget_set_vexpand (l, TRUE);
          gtk_widget_set_valign (l, GTK_ALIGN_START);
          gtk_widget_set_margin_top (l, 8);
          gtk_box_append (GTK_BOX (U.month_wn), l);
        }

      GtkWidget *cell = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
      gtk_widget_add_css_class (cell, "cal-day");
      if (g_date_time_get_month (d) != g_date_time_get_month (first))
        gtk_widget_add_css_class (cell, "other");
      if (day == U.anchor)
        gtk_widget_add_css_class (cell, "selected");
      gtk_widget_set_hexpand (cell, TRUE);
      gtk_widget_set_vexpand (cell, TRUE);
      gtk_widget_set_overflow (cell, GTK_OVERFLOW_HIDDEN);

      /* "1 oct" on the first of a month, like the system calendars */
      g_autofree char *num = g_date_time_get_day_of_month (d) == 1
        ? g_strdup_printf ("%d %s", 1, (english () ? months_short_en : months_short_es)[g_date_time_get_month (d) - 1])
        : g_strdup_printf ("%d", g_date_time_get_day_of_month (d));
      GtkWidget *number = gtk_label_new (num);
      gtk_widget_add_css_class (number, "cal-num");
      if (day == today)
        gtk_widget_add_css_class (number, "today");
      gtk_widget_set_halign (number, GTK_ALIGN_END);
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
              GtkWidget *chip = make_chip (o, TRUE);
              if (o->start < day)
                {
                  /* a multi-day event carries on: a bar, with the title again at the start of each week */
                  gtk_widget_add_css_class (chip, "cont");
                  if (col != 0)
                    gtk_label_set_text (GTK_LABEL (chip), "");
                }
              gtk_box_append (GTK_BOX (cell), chip);
              shown++;
            }
        }
      if (total > shown)
        {
          g_autofree char *more = g_strdup_printf (TR ("%u más…", "%u more…"), total - shown);
          GtkWidget *l = gtk_label_new (more);
          gtk_widget_add_css_class (l, "cal-more");
          gtk_widget_set_halign (l, GTK_ALIGN_START);
          gtk_box_append (GTK_BOX (cell), l);
        }

      GtkGesture *click = gtk_gesture_click_new ();
      gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click), GDK_BUTTON_PRIMARY);
      g_signal_connect_data (click, "pressed", G_CALLBACK (on_day_clicked), g_memdup2 (&day, sizeof day), free_data, 0);
      gtk_widget_add_controller (cell, GTK_EVENT_CONTROLLER (click));
      GtkGesture *right = gtk_gesture_click_new ();
      gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (right), GDK_BUTTON_SECONDARY);
      g_signal_connect_data (right, "pressed", G_CALLBACK (on_day_right), g_memdup2 (&day, sizeof day), free_data, 0);
      gtk_widget_add_controller (cell, GTK_EVENT_CONTROLLER (right));
      gtk_grid_attach (GTK_GRID (U.month_grid), cell, col, i / 7, 1, 1);
    }
  g_array_free (occ, TRUE);
  gtk_widget_set_visible (U.month_wn, cs->week_numbers);
  gtk_widget_set_size_request (U.month_wn, cs->week_numbers ? 28 : 0, -1);

  clear_box (U.month_weekdays);
  if (cs->week_numbers)
    {
      GtkWidget *sp = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
      gtk_widget_set_size_request (sp, 28, -1);
      gtk_box_append (GTK_BOX (U.month_weekdays), sp);
    }
  GtkWidget *names = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_set_homogeneous (GTK_BOX (names), TRUE);
  gtk_widget_set_hexpand (names, TRUE);
  gtk_box_append (GTK_BOX (U.month_weekdays), names);
  for (int i = 0; i < ncols; i++)
    {
      int dow = (week_start () + i) % 7;
      GtkWidget *l = gtk_label_new (english () ? days_en[dow] : days_es[dow]);
      gtk_widget_add_css_class (l, "cal-weekday");
      gtk_widget_set_hexpand (l, TRUE);
      gtk_widget_set_halign (l, GTK_ALIGN_END);
      gtk_widget_set_margin_end (l, 8);
      gtk_box_append (GTK_BOX (names), l);
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

/* which of the n columns a moment falls in; moments outside the range clamp to its ends */
static int
column_of (gint64 first, int n, gint64 t)
{
  for (int i = 0; i < n; i++)
    if (t < shift_days (first, i + 1))
      return i;
  return n - 1;
}

static void
rebuild_week (void)
{
  const CalSettings *cs = calendar_settings ();
  int n = U.mode == VIEW_DAY ? 1 : cs->days;
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
  GtkWidget *strips = gtk_grid_new ();
  gtk_grid_set_column_homogeneous (GTK_GRID (strips), TRUE);
  gtk_grid_set_row_spacing (GTK_GRID (strips), 2);
  gtk_widget_set_hexpand (strips, TRUE);
  gtk_box_append (GTK_BOX (U.week_allday), strips);

  for (int i = 0; i < n; i++)
    {
      gint64 day = shift_days (first, i);
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
    }

  /* all-day events as bars: one that lasts several days is a single bar across them */
  GArray *occ = calendar_occurrences (calendar_default (), first, end);
  guint rows[16] = { 0 };
  int nrows = 0;
  for (guint k = 0; k < occ->len; k++)
    {
      const CalOccurrence *o = &g_array_index (occ, CalOccurrence, k);
      if (!o->event->all_day || o->start >= end || o->end <= first || !occurrence_visible (o))
        continue;
      int c0 = column_of (first, n, o->start), c1 = column_of (first, n, o->end - 1);
      guint mask = ((1u << (c1 - c0 + 1)) - 1) << c0;
      int row = 0;
      while (row < 15 && (rows[row] & mask))
        row++;
      rows[row] |= mask;
      nrows = MAX (nrows, row + 1);
      GtkWidget *chip = make_chip (o, FALSE);
      gtk_widget_set_margin_start (chip, 2);
      gtk_widget_set_margin_end (chip, 2);
      if (c1 > c0)
        gtk_widget_add_css_class (chip, "span");
      gtk_grid_attach (GTK_GRID (strips), chip, c0, row, c1 - c0 + 1, 1);
    }
  g_array_free (occ, TRUE);
  gtk_widget_set_visible (U.week_allday, nrows > 0);

  cal_grid_set_days (U.time_grid, first, n);
  cal_grid_set_hours (U.time_grid, cs->day_start, cs->day_end);
  if (!U.scrolled)
    {
      /* start the view at the working day, or an hour before now when looking at today */
      g_autoptr (GDateTime) nowd = g_date_time_new_now_local ();
      int hour = (first <= today && today < end) ? MAX (g_date_time_get_hour (nowd) - 1, 0) : MAX (cs->day_start - 1, 0);
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
on_grid_context (const char *id, gint64 occ_start, gint64 time, GtkWidget *grid, double x, double y, gpointer data)
{
  (void) data;
  if (id)
    menu_for_event (grid, x, y, id, occ_start);
  else
    menu_for_time (grid, x, y, time, TRUE, U.mode == VIEW_WEEK);
}

static void
on_grid_toggled (const char *id, gint64 occ_start, gpointer data)
{
  (void) data;
  const CalEvent *ev = calendar_find (calendar_default (), id);
  if (ev)
    calendar_set_done (calendar_default (), id, occ_start, !calendar_is_done (ev, occ_start));
  refresh_all ();
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
  if (U.mode == VIEW_YEAR)
    {
      /* picking a day in the year goes to that day */
      U.mode = VIEW_DAY;
      gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (U.toggle[VIEW_DAY]), TRUE);
      return;
    }
  refresh_all ();
}
/* A small month of day buttons. highlight marks the day or week that is on screen. */
static GtkWidget *
month_widget (gint64 m0, gboolean highlight)
{
  GtkWidget *grid = gtk_grid_new ();
  gtk_grid_set_column_homogeneous (GTK_GRID (grid), TRUE);
  g_autoptr (GDateTime) first = local_dt (m0);
  for (int i = 0; i < 7; i++)
    {
      int dow = (week_start () + i) % 7;
      g_autofree char *letter = g_strndup (english () ? days_en[dow] : days_es[dow], 1);
      letter[0] = (char) g_ascii_toupper (letter[0]);
      GtkWidget *l = gtk_label_new (letter);
      gtk_widget_add_css_class (l, "cal-weekday");
      gtk_grid_attach (GTK_GRID (grid), l, i, 0, 1, 1);
    }
  int offset = (g_date_time_get_day_of_week (first) - 1 - week_start () + 7) % 7;
  gint64 start = shift_days (m0, -offset);
  gint64 today = calendar_day_start (now_unix ());
  gint64 sel_first = U.mode == VIEW_WEEK ? week_first (U.anchor) : U.anchor;
  gint64 sel_end = U.mode == VIEW_WEEK ? shift_days (sel_first, 7) : U.mode == VIEW_DAY ? calendar_day_next (U.anchor) : 0;
  int weeks = 6;
  for (int i = 0; i < weeks * 7; i++)
    {
      gint64 day = shift_days (start, i);
      g_autoptr (GDateTime) d = local_dt (day);
      gboolean in_month = g_date_time_get_month (d) == g_date_time_get_month (first);
      if (!in_month)
        {
          /* days of the neighbouring months stay blank, so every month has the same shape */
          gtk_grid_attach (GTK_GRID (grid), gtk_label_new (""), i % 7, 1 + i / 7, 1, 1);
          continue;
        }
      g_autofree char *num = g_strdup_printf ("%d", g_date_time_get_day_of_month (d));
      GtkWidget *b = gtk_button_new_with_label (num);
      gtk_widget_add_css_class (b, "flat");
      gtk_widget_add_css_class (b, "mini-day");
      if (day == today)
        gtk_widget_add_css_class (b, "today");
      if (highlight && sel_end && day >= sel_first && day < sel_end)
        gtk_widget_add_css_class (b, "selected");
      g_object_set_data_full (G_OBJECT (b), "day", g_memdup2 (&day, sizeof day), g_free);
      g_signal_connect (b, "clicked", G_CALLBACK (on_mini_day), NULL);
      GtkGesture *right = gtk_gesture_click_new ();
      gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (right), GDK_BUTTON_SECONDARY);
      g_signal_connect_data (right, "pressed", G_CALLBACK (on_day_right), g_memdup2 (&day, sizeof day), free_data, 0);
      gtk_widget_add_controller (b, GTK_EVENT_CONTROLLER (right));
      gtk_grid_attach (GTK_GRID (grid), b, i % 7, 1 + i / 7, 1, 1);
    }
  return grid;
}

static void
on_year_month_clicked (GtkButton *b, gpointer data)
{
  (void) data;
  gint64 *m0 = g_object_get_data (G_OBJECT (b), "month");
  U.anchor = *m0;
  U.mode = VIEW_MONTH;
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (U.toggle[VIEW_MONTH]), TRUE);
}

static void
rebuild_year (void)
{
  GtkWidget *kid;
  while ((kid = gtk_widget_get_first_child (U.year_grid)))
    gtk_grid_remove (GTK_GRID (U.year_grid), kid);
  g_autoptr (GDateTime) a = local_dt (U.anchor);
  int year = g_date_time_get_year (a);
  gint64 this_month = month_start (now_unix ());
  for (int m = 1; m <= 12; m++)
    {
      g_autoptr (GDateTime) d = g_date_time_new_local (year, m, 1, 0, 0, 0);
      gint64 m0 = g_date_time_to_unix (d);
      GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
      gtk_widget_add_css_class (box, "year-month");
      GtkWidget *title = gtk_button_new_with_label ((english () ? months_en : months_es)[m - 1]);
      gtk_widget_add_css_class (title, "flat");
      gtk_widget_add_css_class (title, "year-title");
      if (m0 == this_month)
        gtk_widget_add_css_class (title, "current");
      gtk_widget_set_halign (title, GTK_ALIGN_START);
      g_object_set_data_full (G_OBJECT (title), "month", g_memdup2 (&m0, sizeof m0), g_free);
      g_signal_connect (title, "clicked", G_CALLBACK (on_year_month_clicked), NULL);
      gtk_box_append (GTK_BOX (box), title);
      gtk_box_append (GTK_BOX (box), month_widget (m0, FALSE));
      gtk_grid_attach (GTK_GRID (U.year_grid), box, (m - 1) % 3, (m - 1) / 3, 1, 1);
    }
}

static void
rebuild_mini (void)
{
  clear_box (U.mini_box);
  gint64 m0 = month_start (U.anchor);
  g_autoptr (GDateTime) first = local_dt (m0);
  g_autofree char *title = g_strdup_printf ("%s %d", (english () ? months_en : months_es)[g_date_time_get_month (first) - 1],
                                            g_date_time_get_year (first));
  title[0] = (char) g_ascii_toupper (title[0]);
  gtk_label_set_text (GTK_LABEL (U.mini_title), title);
  gtk_box_append (GTK_BOX (U.mini_box), month_widget (m0, TRUE));
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

static void
on_cal_refresh (GtkButton *b, gpointer data)
{
  (void) b;
  calendar_subscription_fetch (data, refresh_done, NULL);
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

  if (c->url)
    {
      GtkWidget *info = gtk_label_new (c->url);
      gtk_label_set_ellipsize (GTK_LABEL (info), PANGO_ELLIPSIZE_MIDDLE);
      gtk_label_set_max_width_chars (GTK_LABEL (info), 28);
      gtk_widget_add_css_class (info, "dim-label");
      gtk_box_append (GTK_BOX (box), info);
      GtkWidget *now = gtk_button_new_with_label (TR ("Actualizar ahora", "Refresh now"));
      g_signal_connect_data (now, "clicked", G_CALLBACK (on_cal_refresh), g_strdup (c->id), free_data, 0);
      gtk_box_append (GTK_BOX (box), now);
    }
  if (calendar_calendars (calendar_default ())->len > 1)
    {
      GtkWidget *del = gtk_button_new_with_label (c->url ? TR ("Dejar de seguir", "Unsubscribe")
                                                          : c->account ? TR ("Dejar de sincronizar y borrar aquí", "Stop syncing and delete here")
                                                                       : TR ("Borrar calendario", "Delete calendar"));
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
      if (c->account)
        {
          const CalAccount *acc = caldav_account_find (c->account);
          GtkWidget *cloud = gtk_label_new (acc ? acc->name : "");
          gtk_widget_add_css_class (cloud, "dim-label");
          gtk_widget_add_css_class (cloud, "caption");
          gtk_box_append (GTK_BOX (row), cloud);
        }
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
  if (U.mode == VIEW_YEAR)
    return g_strdup_printf ("<b>%d</b>", year);
  if (U.mode == VIEW_MONTH || U.mode == VIEW_WEEK)
    {
      g_autofree char *m = g_strdup ((english () ? months_en : months_es)[month - 1]);
      m[0] = (char) g_ascii_toupper (m[0]);
      if (U.mode == VIEW_WEEK)
        {
          gint64 a = week_first (U.anchor), b = shift_days (a, 6);
          g_autoptr (GDateTime) da = local_dt (a);
          g_autoptr (GDateTime) db = local_dt (b);
          if (g_date_time_get_month (da) != g_date_time_get_month (db))
            {
              const char **ms = english () ? months_short_en : months_short_es;
              return g_strdup_printf ("<b>%s – %s</b> %d", ms[g_date_time_get_month (da) - 1], ms[g_date_time_get_month (db) - 1],
                                      g_date_time_get_year (db));
            }
        }
      if (U.mode == VIEW_WEEK && calendar_settings ()->week_numbers)
        {
          g_autoptr (GDateTime) mid = local_dt (shift_days (week_first (U.anchor), 3));
          return g_strdup_printf ("<b>%s</b> %d  <span alpha=\"55%%\" size=\"small\">%s %d</span>", m, year, TR ("Semana", "Week"),
                                  g_date_time_get_week_of_year (mid));
        }
      return g_strdup_printf ("<b>%s</b> %d", m, year);
    }
  g_autofree char *day = format_day (U.anchor);
  day[0] = (char) g_ascii_toupper (day[0]);
  return g_strdup_printf ("<b>%s</b> %d", day, year);
}
static void
refresh_all (void)
{
  calendar_alerts_reschedule (); /* every change goes through here */
  if (!U.view)
    return;
  g_autofree char *t = title_text ();
  gtk_label_set_markup (GTK_LABEL (U.title), t);
  rebuild_mini ();
  rebuild_calendars ();
  if (U.mode == VIEW_YEAR)
    {
      gtk_stack_set_visible_child_name (GTK_STACK (U.stack), "year");
      rebuild_year ();
    }
  else if (U.mode == VIEW_MONTH)
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
  U.anchor = U.mode == VIEW_YEAR ? shift_months (U.anchor, 12 * dir)
           : U.mode == VIEW_MONTH ? shift_months (month_start (U.anchor), dir)
           : shift_days (U.anchor, dir * (U.mode == VIEW_WEEK ? 7 : 1));
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
  GString *out = g_string_new (NULL);
  g_string_append_printf (out, "%s%s  ·  %s  ·  %s", q->reminder ? "○ " : "", title, day, q->reminder && !q->all_day ? a : when);
  if (rep)
    {
      g_string_append_printf (out, "  ·  %s", rep);
      if (q->interval > 1)
        g_string_append_printf (out, " ×%u", q->interval);
      if (q->count)
        g_string_append_printf (out, "  ·  %d %s", q->count, TR ("veces", "times"));
    }
  if (q->alert >= 0)
    g_string_append_printf (out, "  ·  %s %d min", TR ("aviso", "alert"), q->alert);
  return g_string_free (out, FALSE);
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
  CalEvent *ev = calendar_quick_to_event (&q, TR ("Sin título", "Untitled"));
  apply_defaults (ev);
  calendar_add (calendar_default (), ev);
  U.anchor = calendar_day_start (q.start);
  calendar_quick_clear (&q);
  gtk_editable_set_text (GTK_EDITABLE (entry), "");
  gtk_menu_button_popdown (GTK_MENU_BUTTON (U.new_button));
  refresh_all ();
}

/* "More options": the full editor, with whatever was typed so far */
static void
on_quick_more (GtkButton *b, gpointer data)
{
  (void) b; (void) data;
  const char *text = gtk_editable_get_text (GTK_EDITABLE (U.quick));
  CalQuick q;
  gint64 start = 0, end = 0;
  if (*text && calendar_quick_parse (text, now_unix (), quick_month_first (), &q))
    {
      g_free (editor_title_hint);
      editor_title_hint = *q.title ? g_strdup (q.title) : NULL;
      start = q.all_day ? 0 : q.start;
      end = q.end;
      if (q.all_day)
        U.anchor = calendar_day_start (q.start);
      calendar_quick_clear (&q);
    }
  gtk_editable_set_text (GTK_EDITABLE (U.quick), "");
  gtk_menu_button_popdown (GTK_MENU_BUTTON (U.new_button));
  open_editor (NULL, 0, start, end);
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
  GtkWidget *side = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_set_size_request (side, 236, -1);
  gtk_widget_set_hexpand (side, FALSE);
  gtk_widget_add_css_class (side, "cal-sidebar");
  gtk_widget_set_margin_start (side, 14);
  gtk_widget_set_margin_end (side, 10);
  gtk_widget_set_margin_top (side, 6);
  gtk_widget_set_margin_bottom (side, 12);

  GtkWidget *cal_head = gtk_label_new (TR ("Calendarios", "Calendars"));
  gtk_widget_add_css_class (cal_head, "cal-section");
  gtk_label_set_xalign (GTK_LABEL (cal_head), 0);
  gtk_box_append (GTK_BOX (side), cal_head);
  U.cal_list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_box_append (GTK_BOX (side), U.cal_list);
  GtkWidget *add = gtk_button_new_with_label (TR ("Nuevo calendario", "New calendar"));
  gtk_widget_add_css_class (add, "flat");
  gtk_widget_set_halign (add, GTK_ALIGN_START);
  g_signal_connect (add, "clicked", G_CALLBACK (on_new_calendar), NULL);
  gtk_box_append (GTK_BOX (side), add);

  GtkWidget *spacer = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_vexpand (spacer, TRUE);
  gtk_box_append (GTK_BOX (side), spacer);

  GtkWidget *mini_head = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 2);
  U.mini_title = gtk_label_new ("");
  gtk_widget_add_css_class (U.mini_title, "cal-section");
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
  U.mini_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append (GTK_BOX (side), U.mini_box);
  return side;
}

/* ---- the settings window ------------------------------------------------ */

static void
add_combo (AdwPreferencesGroup *group, const char *title, const char **items, guint selected, GCallback cb)
{
  GtkWidget *row = adw_combo_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
  g_autoptr (GtkStringList) list = gtk_string_list_new (items);
  adw_combo_row_set_model (ADW_COMBO_ROW (row), G_LIST_MODEL (list));
  adw_combo_row_set_selected (ADW_COMBO_ROW (row), selected);
  g_signal_connect (row, "notify::selected", cb, NULL);
  adw_preferences_group_add (group, row);
}

static void
settings_changed (void)
{
  calendar_settings_save ();
  U.scrolled = FALSE;
  refresh_all ();
}

static void
on_set_first_day (GObject *row, GParamSpec *p, gpointer d)
{
  (void) p; (void) d;
  calendar_settings ()->first_day = (int) adw_combo_row_get_selected (ADW_COMBO_ROW (row));
  settings_changed ();
}

static void
on_set_days (GObject *row, GParamSpec *p, gpointer d)
{
  (void) p; (void) d;
  calendar_settings ()->days = adw_combo_row_get_selected (ADW_COMBO_ROW (row)) == 1 ? 5 : 7;
  settings_changed ();
}

static void
on_set_week_numbers (GObject *row, GParamSpec *p, gpointer d)
{
  (void) p; (void) d;
  calendar_settings ()->week_numbers = adw_switch_row_get_active (ADW_SWITCH_ROW (row));
  settings_changed ();
}

static void
on_set_day_start (GObject *row, GParamSpec *p, gpointer d)
{
  (void) p; (void) d;
  CalSettings *cs = calendar_settings ();
  cs->day_start = (int) adw_spin_row_get_value (ADW_SPIN_ROW (row));
  if (cs->day_end <= cs->day_start)
    cs->day_end = cs->day_start + 1;
  settings_changed ();
}

static void
on_set_day_end (GObject *row, GParamSpec *p, gpointer d)
{
  (void) p; (void) d;
  CalSettings *cs = calendar_settings ();
  cs->day_end = MAX ((int) adw_spin_row_get_value (ADW_SPIN_ROW (row)), cs->day_start + 1);
  settings_changed ();
}

static void
on_set_alert (GObject *row, GParamSpec *p, gpointer d)
{
  (void) p; (void) d;
  calendar_settings ()->default_alert = alert_minutes[adw_combo_row_get_selected (ADW_COMBO_ROW (row))];
  calendar_settings_save ();
}

static void
on_set_default_calendar (GObject *row, GParamSpec *p, gpointer d)
{
  (void) p; (void) d;
  guint i = adw_combo_row_get_selected (ADW_COMBO_ROW (row));
  CalSettings *cs = calendar_settings ();
  g_clear_pointer (&cs->default_calendar, g_free);
  g_autoptr (GPtrArray) own = own_calendars ();
  if (i > 0 && i - 1 < own->len)
    cs->default_calendar = g_strdup (((CalCalendar *) own->pdata[i - 1])->id);
  calendar_settings_save ();
}

static void
on_set_theme (GObject *row, GParamSpec *p, gpointer d)
{
  (void) p; (void) d;
  static const char *ids[] = { "system", "light", "dark" };
  const char *theme = ids[MIN (adw_combo_row_get_selected (ADW_COMBO_ROW (row)), 2u)];
  apply_theme (theme);
  g_strlcpy (calendar_settings ()->theme, theme, sizeof calendar_settings ()->theme);
  calendar_settings_save ();
}

static void
on_set_background (GObject *row, GParamSpec *p, gpointer d)
{
  (void) p; (void) d;
  CalSettings *cs = calendar_settings ();
  cs->background = adw_switch_row_get_active (ADW_SWITCH_ROW (row));
  if (!cs->background)
    cs->autostart = FALSE;
  calendar_settings_save ();
  if (background_handler)
    background_handler (cs->background, cs->autostart);
}

static void
on_set_autostart (GObject *row, GParamSpec *p, gpointer d)
{
  (void) p; (void) d;
  CalSettings *cs = calendar_settings ();
  cs->autostart = adw_switch_row_get_active (ADW_SWITCH_ROW (row));
  if (cs->autostart)
    cs->background = TRUE;
  calendar_settings_save ();
  if (background_handler)
    background_handler (cs->background, cs->autostart);
}

static void
open_settings (void)
{
  const CalSettings *cs = calendar_settings ();
  AdwDialog *dialog = adw_preferences_dialog_new ();
  adw_dialog_set_title (dialog, TR ("Ajustes del calendario", "Calendar settings"));
  GtkWidget *page = adw_preferences_page_new ();

  AdwPreferencesGroup *week = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (week, TR ("Semana", "Week"));
  const char *firsts[] = { TR ("Según el idioma", "Follow the language"), TR ("Lunes", "Monday"), TR ("Martes", "Tuesday"),
                           TR ("Miércoles", "Wednesday"), TR ("Jueves", "Thursday"), TR ("Viernes", "Friday"),
                           TR ("Sábado", "Saturday"), TR ("Domingo", "Sunday"), NULL };
  add_combo (week, TR ("La semana empieza el", "The week starts on"), firsts, (guint) cs->first_day, G_CALLBACK (on_set_first_day));
  const char *days[] = { TR ("Siete días", "Seven days"), TR ("De lunes a viernes", "Monday to Friday"), NULL };
  add_combo (week, TR ("Días que se muestran", "Days shown"), days, cs->days == 5 ? 1 : 0, G_CALLBACK (on_set_days));
  GtkWidget *wn = adw_switch_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (wn), TR ("Mostrar el número de semana", "Show week numbers"));
  adw_switch_row_set_active (ADW_SWITCH_ROW (wn), cs->week_numbers);
  g_signal_connect (wn, "notify::active", G_CALLBACK (on_set_week_numbers), NULL);
  adw_preferences_group_add (week, wn);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), week);

  AdwPreferencesGroup *day = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (day, TR ("Tu día", "Your day"));
  adw_preferences_group_set_description (day, TR ("Las horas fuera de tu día se ven más oscuras y la vista empieza donde empieza tu día.",
                                                  "Hours outside your day look darker, and the view starts where your day starts."));
  GtkWidget *ds = adw_spin_row_new_with_range (0, 22, 1);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (ds), TR ("Empieza a las", "Starts at"));
  adw_spin_row_set_value (ADW_SPIN_ROW (ds), cs->day_start);
  g_signal_connect (ds, "notify::value", G_CALLBACK (on_set_day_start), NULL);
  adw_preferences_group_add (day, ds);
  GtkWidget *de = adw_spin_row_new_with_range (1, 24, 1);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (de), TR ("Termina a las", "Ends at"));
  adw_spin_row_set_value (ADW_SPIN_ROW (de), cs->day_end);
  g_signal_connect (de, "notify::value", G_CALLBACK (on_set_day_end), NULL);
  adw_preferences_group_add (day, de);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), day);

  AdwPreferencesGroup *neu = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (neu, TR ("Eventos nuevos", "New events"));
  g_autoptr (GtkStringList) choices = alert_choices ();
  GtkWidget *al = adw_combo_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (al), TR ("Aviso por defecto", "Default alert"));
  adw_combo_row_set_model (ADW_COMBO_ROW (al), G_LIST_MODEL (choices));
  adw_combo_row_set_selected (ADW_COMBO_ROW (al), alert_index (cs->default_alert));
  g_signal_connect (al, "notify::selected", G_CALLBACK (on_set_alert), NULL);
  adw_preferences_group_add (neu, al);
  g_autoptr (GPtrArray) own = own_calendars ();
  g_autoptr (GtkStringList) names = gtk_string_list_new (NULL);
  gtk_string_list_append (names, TR ("El primero", "The first one"));
  guint sel = 0;
  for (guint i = 0; i < own->len; i++)
    {
      const CalCalendar *c = own->pdata[i];
      gtk_string_list_append (names, c->name);
      if (cs->default_calendar && g_str_equal (c->id, cs->default_calendar))
        sel = i + 1;
    }
  GtkWidget *dc = adw_combo_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (dc), TR ("Calendario por defecto", "Default calendar"));
  adw_combo_row_set_model (ADW_COMBO_ROW (dc), G_LIST_MODEL (names));
  adw_combo_row_set_selected (ADW_COMBO_ROW (dc), sel);
  g_signal_connect (dc, "notify::selected", G_CALLBACK (on_set_default_calendar), NULL);
  adw_preferences_group_add (neu, dc);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), neu);

  if (standalone)
    {
      AdwPreferencesGroup *look = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
      adw_preferences_group_set_title (look, TR ("Apariencia", "Appearance"));
      const char *themes[] = { TR ("Sistema", "System"), TR ("Claro", "Light"), TR ("Oscuro", "Dark"), NULL };
      add_combo (look, TR ("Tema", "Theme"), themes, g_str_equal (cs->theme, "light") ? 1 : g_str_equal (cs->theme, "dark") ? 2 : 0,
                 G_CALLBACK (on_set_theme));
      adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), look);

      AdwPreferencesGroup *bg = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
      adw_preferences_group_set_title (bg, TR ("Avisos", "Alerts"));
      adw_preferences_group_set_description (bg, TR ("Para que avise aunque cierres la ventana. Solo guarda un temporizador, casi no gasta batería ni memoria.",
                                                     "So it still alerts after you close the window. It only keeps one timer, using almost no battery or memory."));
      GtkWidget *keep = adw_switch_row_new ();
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (keep), TR ("Seguir avisando con la ventana cerrada", "Keep alerting with the window closed"));
      adw_switch_row_set_active (ADW_SWITCH_ROW (keep), cs->background);
      g_signal_connect (keep, "notify::active", G_CALLBACK (on_set_background), NULL);
      adw_preferences_group_add (bg, keep);
      GtkWidget *auto_row = adw_switch_row_new ();
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (auto_row), TR ("Empezar al iniciar sesión", "Start when I log in"));
      adw_action_row_set_subtitle (ADW_ACTION_ROW (auto_row), TR ("Arranca en segundo plano, sin abrir la ventana", "Starts in the background, without opening the window"));
      adw_switch_row_set_active (ADW_SWITCH_ROW (auto_row), cs->autostart);
      g_signal_connect (auto_row, "notify::active", G_CALLBACK (on_set_autostart), NULL);
      adw_preferences_group_add (bg, auto_row);
      adw_preferences_page_add (ADW_PREFERENCES_PAGE (page), bg);
    }
  adw_preferences_dialog_add (ADW_PREFERENCES_DIALOG (dialog), ADW_PREFERENCES_PAGE (page));
  adw_dialog_present (dialog, U.view);
}

/* ---- import, export, subscriptions and holidays ------------------------- */

static void
tell (const char *heading, const char *body)
{
  AdwDialog *d = adw_alert_dialog_new (heading, body);
  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (d), "ok", TR ("Aceptar", "OK"));
  adw_dialog_present (d, U.view);
}

static GtkWidget *
labelled_entry (GtkWidget *box, const char *label, const char *placeholder)
{
  GtkWidget *l = gtk_label_new (label);
  gtk_label_set_xalign (GTK_LABEL (l), 0);
  gtk_widget_add_css_class (l, "cal-section");
  gtk_box_append (GTK_BOX (box), l);
  GtkWidget *e = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (e), placeholder);
  gtk_entry_set_activates_default (GTK_ENTRY (e), TRUE);
  gtk_box_append (GTK_BOX (box), e);
  return e;
}

/* ---- import ---- */

typedef struct {
  GPtrArray *events;   /* parsed, waiting for the user to pick where they go */
  char      *name;     /* the file's name, for a new calendar */
  GtkWidget *combo;
  GPtrArray *own;
} ImportAsk;

static void
import_ask_free (gpointer p)
{
  ImportAsk *a = p;
  g_ptr_array_unref (a->events);
  g_ptr_array_unref (a->own);
  g_free (a->name);
  g_free (a);
}

static void
on_import_response (AdwAlertDialog *dialog, const char *response, gpointer data)
{
  (void) dialog;
  ImportAsk *a = data;
  if (!g_str_equal (response, "import"))
    return;
  guint sel = gtk_drop_down_get_selected (GTK_DROP_DOWN (a->combo));
  g_autofree char *target = NULL;
  if (sel == 0)
    target = g_strdup (calendar_calendar_add (calendar_default (), a->name, calendar_calendars (calendar_default ())->len));
  else if (sel - 1 < a->own->len)
    target = g_strdup (((CalCalendar *) a->own->pdata[sel - 1])->id);
  else
    return;
  guint n = calendar_import (calendar_default (), a->events, target);
  refresh_all ();
  g_autofree char *msg = g_strdup_printf (TR ("Se importaron %u eventos.", "%u events were imported."), n);
  tell (TR ("Importación lista", "Import done"), msg);
}

static void
on_import_file (GObject *source, GAsyncResult *res, gpointer data)
{
  (void) data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GFile) file = gtk_file_dialog_open_finish (GTK_FILE_DIALOG (source), res, &error);
  if (!file)
    return;
  g_autofree char *contents = NULL;
  gsize len = 0;
  if (!g_file_load_contents (file, NULL, &contents, &len, NULL, &error) || len > 32 * 1024 * 1024)
    {
      tell (TR ("No se pudo leer el archivo", "Could not read the file"), error ? error->message : "");
      return;
    }
  GPtrArray *events = calendar_ics_parse (contents);
  if (!events->len)
    {
      g_ptr_array_unref (events);
      tell (TR ("No hay eventos", "No events"), TR ("El archivo no tiene eventos que se puedan importar.", "The file has no events that can be imported."));
      return;
    }
  ImportAsk *a = g_new0 (ImportAsk, 1);
  a->events = events;
  a->own = own_calendars ();
  g_autofree char *base = g_file_get_basename (file);
  char *dot = strrchr (base, '.');
  if (dot)
    *dot = 0;
  a->name = g_strdup (*base ? base : TR ("Importado", "Imported"));

  g_autofree char *body = g_strdup_printf (TR ("El archivo tiene %u eventos. ¿Dónde los pongo?", "The file has %u events. Where should they go?"), events->len);
  AdwDialog *d = adw_alert_dialog_new (TR ("Importar calendario", "Import calendar"), body);
  g_autoptr (GtkStringList) names = gtk_string_list_new (NULL);
  g_autofree char *fresh = g_strdup_printf (TR ("Un calendario nuevo: %s", "A new calendar: %s"), a->name);
  gtk_string_list_append (names, fresh);
  for (guint i = 0; i < a->own->len; i++)
    gtk_string_list_append (names, ((CalCalendar *) a->own->pdata[i])->name);
  a->combo = gtk_drop_down_new (G_LIST_MODEL (names), NULL);
  adw_alert_dialog_set_extra_child (ADW_ALERT_DIALOG (d), a->combo);
  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (d), "cancel", TR ("Cancelar", "Cancel"));
  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (d), "import", TR ("Importar", "Import"));
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (d), "import", ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (d), "import");
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (d), "cancel");
  g_object_set_data_full (G_OBJECT (d), "ask", a, import_ask_free);
  g_signal_connect (d, "response", G_CALLBACK (on_import_response), a);
  adw_dialog_present (d, U.view);
}

static void
open_import (void)
{
  GtkFileDialog *fd = gtk_file_dialog_new ();
  gtk_file_dialog_set_title (fd, TR ("Importar un archivo .ics", "Import an .ics file"));
  GtkFileFilter *filter = gtk_file_filter_new ();
  gtk_file_filter_set_name (filter, TR ("Calendarios (.ics)", "Calendars (.ics)"));
  gtk_file_filter_add_pattern (filter, "*.ics");
  gtk_file_filter_add_pattern (filter, "*.ICS");
  gtk_file_filter_add_mime_type (filter, "text/calendar");
  g_autoptr (GListStore) filters = g_list_store_new (GTK_TYPE_FILE_FILTER);
  g_list_store_append (filters, filter);
  g_object_unref (filter);
  gtk_file_dialog_set_filters (fd, G_LIST_MODEL (filters));
  gtk_file_dialog_open (fd, GTK_WINDOW (gtk_widget_get_root (U.view)), NULL, on_import_file, NULL);
  g_object_unref (fd);
}

/* ---- export ---- */

typedef struct {
  GtkWidget *combo;
  GPtrArray *cals;
} ExportAsk;

static void
export_ask_free (gpointer p)
{
  ExportAsk *a = p;
  g_ptr_array_unref (a->cals);
  g_free (a);
}

static void
on_export_saved (GObject *source, GAsyncResult *res, gpointer data)
{
  char *ics = data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GFile) file = gtk_file_dialog_save_finish (GTK_FILE_DIALOG (source), res, &error);
  if (file)
    {
      if (g_file_replace_contents (file, ics, strlen (ics), NULL, FALSE, G_FILE_CREATE_NONE, NULL, NULL, &error))
        tell (TR ("Exportado", "Exported"), TR ("El archivo está guardado.", "The file is saved."));
      else
        tell (TR ("No se pudo guardar", "Could not save"), error->message);
    }
  g_free (ics);
}

static void
on_export_response (AdwAlertDialog *dialog, const char *response, gpointer data)
{
  (void) dialog;
  ExportAsk *a = data;
  if (!g_str_equal (response, "export"))
    return;
  guint sel = gtk_drop_down_get_selected (GTK_DROP_DOWN (a->combo));
  const CalCalendar *c = sel > 0 && sel - 1 < a->cals->len ? a->cals->pdata[sel - 1] : NULL;
  char *ics = calendar_ics_export (calendar_default (), c ? c->id : NULL);
  g_autofree char *name = g_strdup_printf ("%s.ics", c ? c->name : "calendar");
  GtkFileDialog *fd = gtk_file_dialog_new ();
  gtk_file_dialog_set_title (fd, TR ("Guardar el calendario", "Save the calendar"));
  gtk_file_dialog_set_initial_name (fd, name);
  gtk_file_dialog_save (fd, GTK_WINDOW (gtk_widget_get_root (U.view)), NULL, on_export_saved, ics);
  g_object_unref (fd);
}

static void
open_export (void)
{
  ExportAsk *a = g_new0 (ExportAsk, 1);
  a->cals = g_ptr_array_ref (calendar_calendars (calendar_default ()));
  AdwDialog *d = adw_alert_dialog_new (TR ("Exportar", "Export"), TR ("Se guarda un archivo .ics que abre cualquier calendario, incluido el del iPhone.",
                                                                      "An .ics file is saved, which any calendar app can open, including the iPhone's."));
  g_autoptr (GtkStringList) names = gtk_string_list_new (NULL);
  gtk_string_list_append (names, TR ("Todos los calendarios", "All calendars"));
  for (guint i = 0; i < a->cals->len; i++)
    gtk_string_list_append (names, ((CalCalendar *) a->cals->pdata[i])->name);
  a->combo = gtk_drop_down_new (G_LIST_MODEL (names), NULL);
  adw_alert_dialog_set_extra_child (ADW_ALERT_DIALOG (d), a->combo);
  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (d), "cancel", TR ("Cancelar", "Cancel"));
  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (d), "export", TR ("Exportar…", "Export…"));
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (d), "export", ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (d), "cancel");
  g_object_set_data_full (G_OBJECT (d), "ask", a, export_ask_free);
  g_signal_connect (d, "response", G_CALLBACK (on_export_response), a);
  adw_dialog_present (d, U.view);
}

/* ---- subscribe ---- */

static void
subscribed (const char *calendar_id, guint events, const char *error, gpointer data)
{
  (void) calendar_id; (void) data;
  refresh_all ();
  if (error)
    tell (TR ("No se pudo cargar el calendario", "Could not load the calendar"), error);
  else
    {
      g_autofree char *msg = g_strdup_printf (TR ("Se cargaron %u eventos. Se actualizará solo.", "%u events loaded. It will update itself."), events);
      tell (TR ("Suscrito", "Subscribed"), msg);
    }
}

static void
start_subscription (const char *name, const char *url, int hours)
{
  const char *id = calendar_subscription_add (calendar_default (), name, 5, url, hours);
  g_autofree char *keep = g_strdup (id);
  refresh_all ();
  calendar_subscription_fetch (keep, subscribed, NULL);
}

typedef struct {
  GtkWidget *url, *name, *hours;
} SubscribeAsk;

static void
on_subscribe_response (AdwAlertDialog *dialog, const char *response, gpointer data)
{
  (void) dialog;
  SubscribeAsk *a = data;
  if (!g_str_equal (response, "subscribe"))
    return;
  g_autofree char *url = calendar_subscription_normalize_url (gtk_editable_get_text (GTK_EDITABLE (a->url)));
  if (!url)
    {
      tell (TR ("La dirección no es válida", "That address is not valid"),
            TR ("Debe empezar con https://, http:// o webcal://.", "It must start with https://, http:// or webcal://."));
      return;
    }
  static const int hours[] = { 1, 6, 24, 168 };
  const char *name = gtk_editable_get_text (GTK_EDITABLE (a->name));
  start_subscription (*name ? name : TR ("Calendario suscrito", "Subscribed calendar"), url,
                      hours[MIN (gtk_drop_down_get_selected (GTK_DROP_DOWN (a->hours)), 3u)]);
}

static void
open_subscribe (void)
{
  SubscribeAsk *a = g_new0 (SubscribeAsk, 1);
  AdwDialog *d = adw_alert_dialog_new (TR ("Suscribirse a un calendario", "Subscribe to a calendar"),
                                       TR ("Pega la dirección de un calendario público o compartido (.ics o webcal://). Se lee y se actualiza solo; no se puede editar desde aquí. Es la única conexión a internet del calendario.",
                                           "Paste the address of a public or shared calendar (.ics or webcal://). It is read and kept up to date, and cannot be edited here. It is the calendar's only internet connection."));
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  a->url = labelled_entry (box, TR ("Dirección", "Address"), "https://…/calendar.ics");
  a->name = labelled_entry (box, TR ("Nombre", "Name"), TR ("Mi calendario", "My calendar"));
  GtkWidget *l = gtk_label_new (TR ("Actualizar", "Refresh"));
  gtk_label_set_xalign (GTK_LABEL (l), 0);
  gtk_widget_add_css_class (l, "cal-section");
  gtk_box_append (GTK_BOX (box), l);
  g_autoptr (GtkStringList) every = gtk_string_list_new (NULL);
  gtk_string_list_append (every, TR ("Cada hora", "Every hour"));
  gtk_string_list_append (every, TR ("Cada 6 horas", "Every 6 hours"));
  gtk_string_list_append (every, TR ("Cada día", "Every day"));
  gtk_string_list_append (every, TR ("Cada semana", "Every week"));
  a->hours = gtk_drop_down_new (G_LIST_MODEL (every), NULL);
  gtk_drop_down_set_selected (GTK_DROP_DOWN (a->hours), 2);
  gtk_box_append (GTK_BOX (box), a->hours);
  adw_alert_dialog_set_extra_child (ADW_ALERT_DIALOG (d), box);
  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (d), "cancel", TR ("Cancelar", "Cancel"));
  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (d), "subscribe", TR ("Suscribirse", "Subscribe"));
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (d), "subscribe", ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (d), "subscribe");
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (d), "cancel");
  g_object_set_data_full (G_OBJECT (d), "ask", a, g_free);
  g_signal_connect (d, "response", G_CALLBACK (on_subscribe_response), a);
  adw_dialog_present (d, U.view);
}

/* ---- holidays ---- */

static void
on_holidays_response (AdwAlertDialog *dialog, const char *response, gpointer data)
{
  (void) dialog;
  GtkWidget *combo = data;
  if (!g_str_equal (response, "add"))
    return;
  guint n;
  const HolidayRegion *regions = calendar_holiday_regions (&n);
  guint sel = gtk_drop_down_get_selected (GTK_DROP_DOWN (combo));
  if (sel >= n)
    return;
  g_autofree char *url = calendar_holiday_url (&regions[sel]);
  g_autofree char *name = g_strdup_printf ("%s: %s", TR ("Festivos", "Holidays"), TR (regions[sel].name_es, regions[sel].name_en));
  start_subscription (name, url, 168);
}

static void
open_holidays (void)
{
  AdwDialog *d = adw_alert_dialog_new (TR ("Calendario de festivos", "Holiday calendar"),
                                       TR ("Añade los festivos de un país como un calendario aparte, que se puede ocultar o quitar cuando quieras.",
                                           "Adds a country's holidays as a calendar of their own, which you can hide or remove at any time."));
  guint n;
  const HolidayRegion *regions = calendar_holiday_regions (&n);
  g_autoptr (GtkStringList) names = gtk_string_list_new (NULL);
  for (guint i = 0; i < n; i++)
    gtk_string_list_append (names, TR (regions[i].name_es, regions[i].name_en));
  GtkWidget *combo = gtk_drop_down_new (G_LIST_MODEL (names), NULL);
  adw_alert_dialog_set_extra_child (ADW_ALERT_DIALOG (d), combo);
  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (d), "cancel", TR ("Cancelar", "Cancel"));
  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (d), "add", TR ("Añadir", "Add"));
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (d), "add", ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (d), "cancel");
  g_signal_connect (d, "response", G_CALLBACK (on_holidays_response), combo);
  adw_dialog_present (d, U.view);
}

static void
refresh_done (const char *calendar_id, guint events, const char *error, gpointer data)
{
  (void) calendar_id; (void) events; (void) data;
  refresh_all ();
  if (error)
    tell (TR ("No se pudo actualizar", "Could not refresh"), error);
}


/* ---- go to a date ------------------------------------------------------- */

static void
on_goto_picked (GtkCalendar *calendar, gpointer data)
{
  AdwDialog *dialog = data;
  g_autoptr (GDateTime) d = gtk_calendar_get_date (calendar);
  g_autoptr (GDateTime) m = g_date_time_new_local (g_date_time_get_year (d), g_date_time_get_month (d),
                                                    g_date_time_get_day_of_month (d), 0, 0, 0);
  U.anchor = g_date_time_to_unix (m);
  U.scrolled = FALSE;
  adw_dialog_close (dialog);
  refresh_all ();
}

static void
open_goto (void)
{
  AdwDialog *dialog = adw_dialog_new ();
  adw_dialog_set_title (dialog, TR ("Ir a la fecha", "Go to date"));
  GtkWidget *cal = gtk_calendar_new ();
  g_autoptr (GDateTime) d = local_dt (U.anchor);
  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  gtk_calendar_select_day (GTK_CALENDAR (cal), d);
  G_GNUC_END_IGNORE_DEPRECATIONS
  gtk_widget_set_margin_start (cal, 12);
  gtk_widget_set_margin_end (cal, 12);
  gtk_widget_set_margin_bottom (cal, 12);
  g_signal_connect (cal, "day-selected", G_CALLBACK (on_goto_picked), dialog);
  GtkWidget *tv = adw_toolbar_view_new ();
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (tv), adw_header_bar_new ());
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (tv), cal);
  adw_dialog_set_child (dialog, tv);
  adw_dialog_present (dialog, U.view);
}

/* ---- keyboard: edit, copy, paste, delete and nudge the selected event ---- */

/* the event the user picked last: in the day and week grids, or by clicking a chip or popover */
static const char *
selected_event (gint64 *occ)
{
  gint64 o = 0;
  const char *id = (U.mode == VIEW_DAY || U.mode == VIEW_WEEK) ? cal_grid_selected (U.time_grid, &o) : NULL;
  if (!id && sel_id && calendar_find (calendar_default (), sel_id))
    {
      id = sel_id;
      o = sel_occ;
    }
  if (id && occ)
    *occ = o;
  return id;
}

static void
move_selected (int minutes, int days)
{
  gint64 occ;
  const char *id = selected_event (&occ);
  const CalEvent *ev = id ? calendar_find (calendar_default (), id) : NULL;
  if (!ev || calendar_of (ev)->url)
    return;
  CalEvent *edited = calendar_event_copy (ev);
  gint64 start = days ? shift_days (occ, days) : occ + (gint64) minutes * 60;
  edited->start = start;
  edited->end = start + (ev->end - ev->start);
  edit_showing (id, occ, edited, NULL);
}

static void
copy_selected (void)
{
  gint64 occ;
  const char *id = selected_event (&occ);
  if (!id)
    return;
  calendar_event_free (clipboard_event);
  clipboard_event = calendar_event_copy (calendar_find (calendar_default (), id));
  /* remember the showing's own time, not the first one's */
  clipboard_event->start = occ;
  clipboard_event->end = occ + (clipboard_event->end - calendar_find (calendar_default (), id)->start);
}

static void
paste_event (void)
{
  if (!clipboard_event)
    return;
  CalEvent *ev = calendar_event_copy (clipboard_event);
  gint64 length = ev->end - ev->start;
  gint64 clock = ev->start - calendar_day_start (ev->start);
  g_autoptr (GDateTime) d = local_dt (U.anchor);
  g_autoptr (GDateTime) base = g_date_time_new_local (g_date_time_get_year (d), g_date_time_get_month (d), g_date_time_get_day_of_month (d), 0, 0, 0);
  g_autoptr (GDateTime) at = g_date_time_add_seconds (base, ev->all_day ? 0 : (gdouble) clock);
  ev->start = g_date_time_to_unix (at);
  ev->end = ev->start + length;
  ev->done = FALSE;
  g_clear_pointer (&ev->exceptions, g_array_unref);
  g_clear_pointer (&ev->completed, g_array_unref);
  if (calendar_of (ev)->url)
    {
      g_free (ev->calendar);
      ev->calendar = NULL;
    }
  calendar_add (calendar_default (), ev);
  refresh_all ();
}

static gboolean
on_key (GtkEventControllerKey *c, guint keyval, guint keycode, GdkModifierType state, gpointer data)
{
  (void) c; (void) keycode; (void) data;
  GtkWidget *focus = gtk_root_get_focus (gtk_widget_get_root (U.view));
  gboolean typing = focus && GTK_IS_EDITABLE (focus);

  if (!(state & GDK_CONTROL_MASK))
    {
      if ((keyval == GDK_KEY_Delete || keyval == GDK_KEY_BackSpace) && !typing)
        {
          gint64 occ;
          const char *id = selected_event (&occ);
          const CalEvent *ev = id ? calendar_find (calendar_default (), id) : NULL;
          if (ev && !calendar_of (ev)->url)
            {
              delete_showing (id, occ, NULL);
              return TRUE;
            }
        }
      return FALSE;
    }
  if (state & GDK_ALT_MASK)
    {
      switch (keyval)
        {
        case GDK_KEY_Up:    move_selected (-15, 0); return TRUE;
        case GDK_KEY_Down:  move_selected (15, 0); return TRUE;
        case GDK_KEY_Left:  move_selected (0, -1); return TRUE;
        case GDK_KEY_Right: move_selected (0, 1); return TRUE;
        default:            return FALSE;
        }
    }
  switch (keyval)
    {
    case GDK_KEY_n:     gtk_menu_button_popup (GTK_MENU_BUTTON (U.new_button)); return TRUE;
    case GDK_KEY_f:     gtk_widget_grab_focus (U.search); return TRUE;
    case GDK_KEY_t:     on_today (NULL, NULL); return TRUE;
    case GDK_KEY_T:     open_goto (); return TRUE;
    case GDK_KEY_l:     gtk_widget_grab_focus (U.quick); return TRUE;
    case GDK_KEY_comma: open_settings (); return TRUE;
    case GDK_KEY_r:     calendar_subscriptions_refresh_all (); return TRUE;
    case GDK_KEY_p:     print_view (); return TRUE;
    case GDK_KEY_e:
      {
        gint64 occ;
        const char *id = selected_event (&occ);
        const CalEvent *ev = id ? calendar_find (calendar_default (), id) : NULL;
        if (ev)
          open_editor (ev, occ, 0, 0);
        return ev != NULL;
      }
    case GDK_KEY_c:     if (typing) return FALSE; copy_selected (); return TRUE;
    case GDK_KEY_v:     if (typing) return FALSE; paste_event (); return TRUE;
    case GDK_KEY_1:     gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (U.toggle[VIEW_DAY]), TRUE); return TRUE;
    case GDK_KEY_2:     gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (U.toggle[VIEW_WEEK]), TRUE); return TRUE;
    case GDK_KEY_3:     gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (U.toggle[VIEW_MONTH]), TRUE); return TRUE;
    case GDK_KEY_4:     gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (U.toggle[VIEW_YEAR]), TRUE); return TRUE;
    case GDK_KEY_Left:  on_step (NULL, GINT_TO_POINTER (-1)); return TRUE;
    case GDK_KEY_Right: on_step (NULL, GINT_TO_POINTER (1)); return TRUE;
    default:            return FALSE;
    }
}

static void
on_sidebar_toggled (GtkToggleButton *b, gpointer data)
{
  (void) data;
  gtk_revealer_set_reveal_child (GTK_REVEALER (U.sidebar), gtk_toggle_button_get_active (b));
}

static void
act_settings (GSimpleAction *a, GVariant *p, gpointer d)
{
  (void) a; (void) p; (void) d;
  open_settings ();
}

static void
act_goto (GSimpleAction *a, GVariant *p, gpointer d)
{
  (void) a; (void) p; (void) d;
  open_goto ();
}

static void act_accounts (GSimpleAction *a, GVariant *p, gpointer d) { (void) a; (void) p; (void) d; calendar_accounts_open (U.view); }
static void act_sync (GSimpleAction *a, GVariant *p, gpointer d) { (void) a; (void) p; (void) d; caldav_sync_now (NULL, NULL); }
static void act_import (GSimpleAction *a, GVariant *p, gpointer d) { (void) a; (void) p; (void) d; open_import (); }
static void act_export (GSimpleAction *a, GVariant *p, gpointer d) { (void) a; (void) p; (void) d; open_export (); }
static void act_subscribe (GSimpleAction *a, GVariant *p, gpointer d) { (void) a; (void) p; (void) d; open_subscribe (); }
static void act_holidays (GSimpleAction *a, GVariant *p, gpointer d) { (void) a; (void) p; (void) d; open_holidays (); }

static void
print_view (void)
{
  PrintView v = U.mode == VIEW_DAY ? PRINT_DAY : U.mode == VIEW_WEEK ? PRINT_WEEK : U.mode == VIEW_MONTH ? PRINT_MONTH : PRINT_YEAR;
  calendar_print (GTK_WINDOW (gtk_widget_get_root (U.view)), v, U.anchor, NULL);
}

static void act_print (GSimpleAction *a, GVariant *p, gpointer d) { (void) a; (void) p; (void) d; print_view (); }

static void
act_refresh (GSimpleAction *a, GVariant *p, gpointer d)
{
  (void) a; (void) p; (void) d;
  calendar_subscriptions_refresh_all ();
}

static GtkWidget *
main_menu (void)
{
  GtkWidget *btn = gtk_menu_button_new ();
  gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (btn), "open-menu-symbolic");
  GMenu *menu = g_menu_new ();
  GMenu *nav = g_menu_new ();
  g_menu_append (nav, TR ("Ir a la fecha…", "Go to date…"), "cal.goto");
  g_menu_append_section (menu, NULL, G_MENU_MODEL (nav));
  GMenu *data = g_menu_new ();
  g_menu_append (data, TR ("Cuentas (iPhone, iCloud)…", "Accounts (iPhone, iCloud)…"), "cal.accounts");
  g_menu_append (data, TR ("Sincronizar ahora", "Sync now"), "cal.sync");
  g_menu_append (data, TR ("Importar un archivo .ics…", "Import an .ics file…"), "cal.import");
  g_menu_append (data, TR ("Exportar…", "Export…"), "cal.export");
  g_menu_append (data, TR ("Suscribirse a un calendario…", "Subscribe to a calendar…"), "cal.subscribe");
  g_menu_append (data, TR ("Añadir festivos de un país…", "Add a country's holidays…"), "cal.holidays");
  g_menu_append (data, TR ("Actualizar suscripciones", "Refresh subscriptions"), "cal.refresh");
  g_menu_append (data, TR ("Imprimir…", "Print…"), "cal.print");
  g_menu_append_section (menu, NULL, G_MENU_MODEL (data));
  g_object_unref (data);
  GMenu *app = g_menu_new ();
  g_menu_append (app, TR ("Ajustes", "Settings"), "cal.settings");
  g_menu_append_section (menu, NULL, G_MENU_MODEL (app));
  gtk_menu_button_set_menu_model (GTK_MENU_BUTTON (btn), G_MENU_MODEL (menu));
  g_object_unref (nav);
  g_object_unref (app);
  g_object_unref (menu);
  return btn;
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
  else if (want && g_str_equal (want, "year"))
    U.mode = VIEW_YEAR;
  U.anchor = calendar_day_start (now_unix ());
  U.scrolled = FALSE;
  if (standalone)
    {
      const char *theme = g_getenv ("CALENDAR_THEME");
      apply_theme (theme ? theme : saved_theme ());
    }

  /* ---- toolbar: sidebar, new event, the four views, search ---- */
  GtkWidget *header = adw_header_bar_new ();
  GtkWidget *side_toggle = gtk_toggle_button_new ();
  gtk_button_set_icon_name (GTK_BUTTON (side_toggle), "sidebar-show-symbolic");
  gtk_widget_set_tooltip_text (side_toggle, TR ("Barra lateral", "Sidebar"));
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (side_toggle), TRUE);
  g_signal_connect (side_toggle, "toggled", G_CALLBACK (on_sidebar_toggled), NULL);
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), side_toggle);

  U.new_button = gtk_menu_button_new ();
  gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (U.new_button), "list-add-symbolic");
  gtk_widget_set_tooltip_text (U.new_button, TR ("Nuevo evento (Ctrl+N)", "New event (Ctrl+N)"));
  U.quick = gtk_entry_new ();
  gtk_widget_set_size_request (U.quick, 340, -1);
  gtk_entry_set_placeholder_text (GTK_ENTRY (U.quick),
                                  TR ("Cena con Ana jueves 7pm", "Dinner with Ana thursday 7pm"));
  g_signal_connect (U.quick, "changed", G_CALLBACK (on_quick_changed), NULL);
  g_signal_connect (U.quick, "activate", G_CALLBACK (on_quick_activate), NULL);
  U.quick_preview = gtk_label_new ("");
  gtk_widget_add_css_class (U.quick_preview, "cal-preview");
  gtk_label_set_xalign (GTK_LABEL (U.quick_preview), 0);
  gtk_label_set_wrap (GTK_LABEL (U.quick_preview), TRUE);
  GtkWidget *more = gtk_button_new_with_label (TR ("Más opciones…", "More options…"));
  gtk_widget_add_css_class (more, "flat");
  gtk_widget_set_halign (more, GTK_ALIGN_END);
  g_signal_connect (more, "clicked", G_CALLBACK (on_quick_more), NULL);
  GtkWidget *quick_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_start (quick_box, 8);
  gtk_widget_set_margin_end (quick_box, 8);
  gtk_widget_set_margin_top (quick_box, 8);
  gtk_widget_set_margin_bottom (quick_box, 4);
  GtkWidget *hint = gtk_label_new (TR ("Escribe el evento y pulsa Enter", "Type the event and press Enter"));
  gtk_widget_add_css_class (hint, "cal-section");
  gtk_label_set_xalign (GTK_LABEL (hint), 0);
  gtk_box_append (GTK_BOX (quick_box), hint);
  gtk_box_append (GTK_BOX (quick_box), U.quick);
  gtk_box_append (GTK_BOX (quick_box), U.quick_preview);
  gtk_box_append (GTK_BOX (quick_box), more);
  GtkWidget *quick_pop = gtk_popover_new ();
  gtk_popover_set_child (GTK_POPOVER (quick_pop), quick_box);
  gtk_menu_button_set_popover (GTK_MENU_BUTTON (U.new_button), quick_pop);
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), U.new_button);

  GtkWidget *modes = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (modes, "linked");
  U.toggle[VIEW_DAY] = mode_button (TR ("Día", "Day"), VIEW_DAY, NULL);
  U.toggle[VIEW_WEEK] = mode_button (TR ("Semana", "Week"), VIEW_WEEK, GTK_TOGGLE_BUTTON (U.toggle[VIEW_DAY]));
  U.toggle[VIEW_MONTH] = mode_button (TR ("Mes", "Month"), VIEW_MONTH, GTK_TOGGLE_BUTTON (U.toggle[VIEW_DAY]));
  U.toggle[VIEW_YEAR] = mode_button (TR ("Año", "Year"), VIEW_YEAR, GTK_TOGGLE_BUTTON (U.toggle[VIEW_DAY]));
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (U.toggle[U.mode]), TRUE);
  for (int i = 0; i < 4; i++)
    gtk_box_append (GTK_BOX (modes), U.toggle[i]);
  adw_header_bar_set_title_widget (ADW_HEADER_BAR (header), modes);

  adw_header_bar_pack_end (ADW_HEADER_BAR (header), main_menu ());
  U.search = gtk_search_entry_new ();
  gtk_widget_set_size_request (U.search, 170, -1);
  gtk_search_entry_set_placeholder_text (GTK_SEARCH_ENTRY (U.search), TR ("Buscar", "Search"));
  g_signal_connect (U.search, "search-changed", G_CALLBACK (on_search_changed), NULL);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), U.search);

  /* ---- the big title and the arrows, above the calendar ---- */
  U.title = gtk_label_new ("");
  gtk_widget_add_css_class (U.title, "cal-title");
  gtk_label_set_xalign (GTK_LABEL (U.title), 0);
  gtk_widget_set_hexpand (U.title, TRUE);
  GtkWidget *nav = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (nav, "linked");
  GtkWidget *prev = gtk_button_new_from_icon_name ("go-previous-symbolic");
  GtkWidget *today = gtk_button_new_with_label (TR ("Hoy", "Today"));
  GtkWidget *next = gtk_button_new_from_icon_name ("go-next-symbolic");
  gtk_widget_set_tooltip_text (prev, TR ("Anterior (Ctrl+←)", "Previous (Ctrl+←)"));
  gtk_widget_set_tooltip_text (next, TR ("Siguiente (Ctrl+→)", "Next (Ctrl+→)"));
  g_signal_connect (prev, "clicked", G_CALLBACK (on_step), GINT_TO_POINTER (-1));
  g_signal_connect (next, "clicked", G_CALLBACK (on_step), GINT_TO_POINTER (1));
  g_signal_connect (today, "clicked", G_CALLBACK (on_today), NULL);
  gtk_box_append (GTK_BOX (nav), prev);
  gtk_box_append (GTK_BOX (nav), today);
  gtk_box_append (GTK_BOX (nav), next);
  GtkWidget *top_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_start (top_row, 20);
  gtk_widget_set_margin_end (top_row, 16);
  gtk_widget_set_margin_top (top_row, 4);
  gtk_widget_set_margin_bottom (top_row, 10);
  gtk_box_append (GTK_BOX (top_row), U.title);
  gtk_box_append (GTK_BOX (top_row), nav);

  /* ---- month page: a flat grid with hairlines ---- */
  GtkWidget *month = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_margin_start (month, 16);
  gtk_widget_set_margin_end (month, 16);
  gtk_widget_set_margin_bottom (month, 16);
  U.month_weekdays = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_set_homogeneous (GTK_BOX (U.month_weekdays), TRUE);
  gtk_box_append (GTK_BOX (month), U.month_weekdays);
  U.month_grid = gtk_grid_new ();
  gtk_grid_set_row_homogeneous (GTK_GRID (U.month_grid), TRUE);
  gtk_grid_set_column_homogeneous (GTK_GRID (U.month_grid), TRUE);
  gtk_widget_add_css_class (U.month_grid, "cal-month");
  gtk_widget_set_vexpand (U.month_grid, TRUE);
  gtk_widget_set_hexpand (U.month_grid, TRUE);
  U.month_wn = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_set_homogeneous (GTK_BOX (U.month_wn), TRUE);
  GtkWidget *month_body = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_append (GTK_BOX (month_body), U.month_wn);
  gtk_box_append (GTK_BOX (month_body), U.month_grid);
  gtk_widget_set_vexpand (month_body, TRUE);
  gtk_box_append (GTK_BOX (month), month_body);

  /* ---- year page ---- */
  U.year_grid = gtk_grid_new ();
  gtk_grid_set_column_homogeneous (GTK_GRID (U.year_grid), TRUE);
  gtk_grid_set_row_homogeneous (GTK_GRID (U.year_grid), TRUE);
  gtk_grid_set_column_spacing (GTK_GRID (U.year_grid), 24);
  gtk_grid_set_row_spacing (GTK_GRID (U.year_grid), 16);
  gtk_widget_set_margin_start (U.year_grid, 20);
  gtk_widget_set_margin_end (U.year_grid, 20);
  gtk_widget_set_margin_bottom (U.year_grid, 16);
  GtkWidget *year_scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (year_scroll), U.year_grid);

  /* ---- week and day page ---- */
  CalGridCallbacks cb = { on_grid_picked, on_grid_open, on_grid_create, on_grid_context, on_grid_toggled, on_grid_moved };
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

  /* ---- search results ---- */
  U.search_list = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (U.search_list), GTK_SELECTION_NONE);
  gtk_widget_add_css_class (U.search_list, "boxed-list");
  U.search_empty = gtk_label_new (TR ("Sin resultados.", "No results."));
  gtk_widget_add_css_class (U.search_empty, "dim-label");
  GtkWidget *results = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start (results, 20);
  gtk_widget_set_margin_end (results, 20);
  gtk_widget_set_margin_bottom (results, 16);
  gtk_box_append (GTK_BOX (results), U.search_list);
  gtk_box_append (GTK_BOX (results), U.search_empty);
  GtkWidget *results_scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (results_scroll), results);

  U.stack = gtk_stack_new ();
  gtk_stack_add_named (GTK_STACK (U.stack), results_scroll, "search");
  gtk_stack_add_named (GTK_STACK (U.stack), week, "week");
  gtk_stack_add_named (GTK_STACK (U.stack), month, "month");
  gtk_stack_add_named (GTK_STACK (U.stack), year_scroll, "year");
  gtk_widget_set_hexpand (U.stack, TRUE);
  gtk_widget_set_vexpand (U.stack, TRUE);

  GtkWidget *main_col = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_hexpand (main_col, TRUE);
  gtk_box_append (GTK_BOX (main_col), top_row);
  gtk_box_append (GTK_BOX (main_col), U.stack);

  U.sidebar = gtk_revealer_new ();
  gtk_revealer_set_transition_type (GTK_REVEALER (U.sidebar), GTK_REVEALER_TRANSITION_TYPE_SLIDE_RIGHT);
  gtk_revealer_set_reveal_child (GTK_REVEALER (U.sidebar), TRUE);
  GtkWidget *side_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_append (GTK_BOX (side_box), build_sidebar ());
  gtk_box_append (GTK_BOX (side_box), gtk_separator_new (GTK_ORIENTATION_VERTICAL));
  gtk_revealer_set_child (GTK_REVEALER (U.sidebar), side_box);

  GtkWidget *body = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_append (GTK_BOX (body), U.sidebar);
  gtk_box_append (GTK_BOX (body), main_col);

  GtkWidget *tv = adw_toolbar_view_new ();
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (tv), header);
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (tv), body);
  GtkEventController *keys = gtk_event_controller_key_new ();
  gtk_event_controller_set_propagation_phase (keys, GTK_PHASE_CAPTURE);
  g_signal_connect (keys, "key-pressed", G_CALLBACK (on_key), NULL);
  gtk_widget_add_controller (tv, keys);
  GSimpleActionGroup *actions = g_simple_action_group_new ();
  GActionEntry entries[] = {
    { "settings", act_settings, NULL, NULL, NULL, { 0 } },
    { "goto", act_goto, NULL, NULL, NULL, { 0 } },
    { "accounts", act_accounts, NULL, NULL, NULL, { 0 } },
    { "sync", act_sync, NULL, NULL, NULL, { 0 } },
    { "import", act_import, NULL, NULL, NULL, { 0 } },
    { "export", act_export, NULL, NULL, NULL, { 0 } },
    { "subscribe", act_subscribe, NULL, NULL, NULL, { 0 } },
    { "holidays", act_holidays, NULL, NULL, NULL, { 0 } },
    { "refresh", act_refresh, NULL, NULL, NULL, { 0 } },
    { "print", act_print, NULL, NULL, NULL, { 0 } },
  };
  g_action_map_add_action_entries (G_ACTION_MAP (actions), entries, G_N_ELEMENTS (entries), NULL);
  gtk_widget_insert_action_group (tv, "cal", G_ACTION_GROUP (actions));
  g_object_unref (actions);
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

/* Waits until the grid has really been drawn again, so its blocks match what the test just did. */
static void
settle (void)
{
  guint before = cal_grid_draw_count (U.time_grid);
  gtk_widget_queue_draw (U.time_grid);
  gint64 deadline = g_get_monotonic_time () + 3 * G_USEC_PER_SEC;
  while (cal_grid_draw_count (U.time_grid) == before && g_get_monotonic_time () < deadline)
    g_main_context_iteration (NULL, TRUE);
}

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
  /* the blocks are laid out during drawing, so wait for a real frame */
  settle ();

  /* move: from the middle of the event to four hours later */
  CHECK (cal_grid_point (U.time_grid, t10 + 1800, &x, &y), "event position");
  cal_grid_point (U.time_grid, t10 + 4 * 3600 + 1800, &x1, &y1);
  cal_grid_test_drag (U.time_grid, x, y, x1, y1);
  const CalEvent *ev = calendar_find (cal, event_id);
  CHECK (ev && ev->start == t10 + 4 * 3600, "move changes the start by four hours");
  CHECK (ev && ev->end - ev->start == 3600, "move keeps the duration");

  /* stretch: drag the bottom edge down one hour */
  settle ();
  gint64 s = ev ? ev->start : t10;
  cal_grid_point (U.time_grid, s + 3600 - 300, &x, &y); /* inside the bottom strip */
  cal_grid_point (U.time_grid, s + 2 * 3600, &x1, &y1);
  cal_grid_test_drag (U.time_grid, x, y, x1, y1);
  ev = calendar_find (cal, event_id);
  CHECK (ev && ev->end == s + 2 * 3600, "stretching the bottom edge adds an hour");
  CHECK (ev && ev->start == s, "stretching keeps the start");

  /* a plain click must not move anything */
  settle ();
  cal_grid_point (U.time_grid, s + 1800, &x, &y);
  cal_grid_test_drag (U.time_grid, x, y, x + 1, y + 1);
  ev = calendar_find (cal, event_id);
  CHECK (ev && ev->start == s, "a click without dragging leaves the event alone");

  /* keyboard: nudge by 15 minutes, copy and paste to another day, delete and bring back */
  {
    settle ();
    const CalEvent *cur = calendar_find (cal, event_id);
    gint64 before = cur ? cur->start : 0;
    move_selected (15, 0);
    cur = calendar_find (cal, event_id);
    CHECK (cur && cur->start == before + 900, "Ctrl+Alt+Down moves the selected event 15 minutes");
    move_selected (0, 1);
    cur = calendar_find (cal, event_id);
    CHECK (cur && calendar_day_start (cur->start) == calendar_day_next (calendar_day_start (before)), "Ctrl+Alt+Right moves it a day");
    move_selected (0, -1);
    move_selected (-15, 0);
    cur = calendar_find (cal, event_id);
    CHECK (cur && cur->start == before, "and back again");
    guint count = calendar_count (cal);
    copy_selected ();
    U.anchor = shift_days (today, 2);
    paste_event ();
    CHECK (calendar_count (cal) == count + 1, "paste adds a copy");
    U.anchor = today;
    on_key (NULL, GDK_KEY_Delete, 0, 0, NULL);
    CHECK (calendar_find (cal, event_id) == NULL, "Delete removes the selected event");
    g_autofree char *gone = NULL;
    CHECK (calendar_undo_delete (cal, &gone) && calendar_find (cal, event_id) != NULL, "and it can be brought back");
    refresh_all ();
    settle ();
    /* the drag steps below need the event selected again */
    cal_grid_point (U.time_grid, before + 1800, &x, &y);
    cal_grid_test_drag (U.time_grid, x, y, x + 1, y + 1);
  }

  /* create: drag across free time from 18:00 to 19:30 */
  cal_grid_point (U.time_grid, today + 18 * 3600, &x, &y);
  cal_grid_point (U.time_grid, today + 19 * 3600 + 1800, &x1, &y1);
  test_create_start = test_create_end = 0;
  cal_grid_test_drag (U.time_grid, x, y, x1, y1);
  CHECK (test_create_start == today + 18 * 3600 && test_create_end == today + 19 * 3600 + 1800, "dragging over free time proposes that range");
  /* right click: the menus open without trouble, and each entry does what it says */
  {
    settle ();
    cal_grid_point (U.time_grid, today + 16 * 3600, &x, &y);
    cal_grid_test_context (U.time_grid, x, y);              /* on free time */
    cal_grid_point (U.time_grid, s + 1800, &x, &y);
    cal_grid_test_context (U.time_grid, x, y);              /* on an event */
    for (int i = 0; i < 20; i++)
      g_main_context_iteration (NULL, FALSE);

    gint64 t14 = today + 14 * 3600;
    Ctx at14 = { t14, NULL, 0, U.time_grid, 0, 0 };
    act_new_event (&at14);
    CHECK (last_editor != NULL && last_editor->day == today && (int) gtk_spin_button_get_value (GTK_SPIN_BUTTON (last_editor->start_h)) == 14 &&
           (int) gtk_spin_button_get_value (GTK_SPIN_BUTTON (last_editor->end_h)) == 15, "new event starts at the clicked time, an hour long");
    if (last_editor)
      adw_dialog_force_close (last_editor->dialog);
    act_new_reminder (&at14);
    CHECK (last_editor != NULL && adw_switch_row_get_active (ADW_SWITCH_ROW (last_editor->reminder)), "new reminder starts as a reminder");
    if (last_editor)
      adw_dialog_force_close (last_editor->dialog);
    act_new_allday (&at14);
    CHECK (last_editor != NULL && adw_switch_row_get_active (ADW_SWITCH_ROW (last_editor->all_day)) && last_editor->day == today,
           "all-day event starts as all day, on that day");
    if (last_editor)
      adw_dialog_force_close (last_editor->dialog);
    act_new_event (&at14);
    CHECK (last_editor && !adw_switch_row_get_active (ADW_SWITCH_ROW (last_editor->reminder)) && !adw_switch_row_get_active (ADW_SWITCH_ROW (last_editor->all_day)),
           "and the presets do not stick to the next editor");
    if (last_editor)
      adw_dialog_force_close (last_editor->dialog);

    Ctx on_event = { 0, (char *) event_id, s, U.time_grid, 0, 0 };
    guint before_dup = calendar_count (cal);
    act_duplicate_event (&on_event);
    CHECK (calendar_count (cal) == before_dup + 1, "duplicate adds a copy");
    act_edit_event (&on_event);
    CHECK (last_editor != NULL && last_editor->id != NULL && g_str_equal (last_editor->id, event_id), "edit opens that event");
    if (last_editor)
      adw_dialog_force_close (last_editor->dialog);

    CalEvent *task = calendar_event_new ("Right-click task", t14, 0, FALSE);
    task->reminder = TRUE;
    g_autofree char *task_id = g_strdup (calendar_add (cal, task));
    Ctx on_task = { 0, task_id, t14, U.time_grid, 0, 0 };
    act_toggle_done (&on_task);
    CHECK (calendar_is_done (calendar_find (cal, task_id), t14), "toggle marks a reminder done");
    act_toggle_done (&on_task);
    CHECK (!calendar_is_done (calendar_find (cal, task_id), t14), "and not done again");
    act_delete_event (&on_task);
    CHECK (calendar_find (cal, task_id) == NULL, "delete removes it");
    g_autofree char *back_title = NULL;
    CHECK (calendar_undo_delete (cal, &back_title), "and it can be undone");

    act_goto_day (&at14);
    CHECK (U.mode == VIEW_DAY, "show this day switches to the day view");
    U.mode = VIEW_WEEK;
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (U.toggle[VIEW_WEEK]), TRUE);
    refresh_all ();
    settle ();
  }

  /* the editor: fill the form and save, then look at what was stored */
  {
    gint64 when = today + 20 * 3600;
    open_editor (NULL, 0, when, when + 3600);
    CHECK (last_editor != NULL, "the editor opens");
    if (last_editor)
      {
        Editor *ed = last_editor;
        gtk_editable_set_text (GTK_EDITABLE (ed->title), "Editor test");
        gtk_editable_set_text (GTK_EDITABLE (ed->location), "Room 7");
        adw_combo_row_set_selected (ADW_COMBO_ROW (ed->repeat), CAL_REPEAT_WEEKLY);
        adw_spin_row_set_value (ADW_SPIN_ROW (ed->interval_row), 2);
        adw_combo_row_set_selected (ADW_COMBO_ROW (ed->ends_row), 2);
        adw_spin_row_set_value (ADW_SPIN_ROW (ed->count_row), 3);
        adw_combo_row_set_selected (ADW_COMBO_ROW (ed->alert1), alert_index (10));
        adw_combo_row_set_selected (ADW_COMBO_ROW (ed->alert2), alert_index (1440));
        adw_switch_row_set_active (ADW_SWITCH_ROW (ed->reminder), TRUE);
        on_editor_save (NULL, ed);
      }
    const CalEvent *saved = NULL;
    GArray *later = calendar_occurrences (cal, when - 60, when + 86400 * 20);
    for (guint i = 0; i < later->len; i++)
      if (g_str_equal (g_array_index (later, CalOccurrence, i).event->title, "Editor test"))
        saved = g_array_index (later, CalOccurrence, i).event;
    CHECK (saved != NULL, "the saved event exists");
    if (saved)
      {
        CHECK (saved->repeat == CAL_REPEAT_WEEKLY && saved->interval == 2 && saved->count == 3, "repeat every 2 weeks, 3 times");
        CHECK (saved->reminder, "it is a reminder");
        CHECK (calendar_event_alert_count (saved) == 2 && calendar_event_alert (saved, 0) == 10 && calendar_event_alert (saved, 1) == 1440,
               "two alerts");
        CHECK (g_str_equal (saved->location, "Room 7"), "location");
      }
    g_array_free (later, TRUE);
  }

  /* saving an event must not change alerts the editor has no menu entry for: iCloud's "15 hours before"
   * on all-day events, "9 hours after the start", and more than two alerts */
  {
    CalEvent *odd = calendar_event_new ("Odd alerts", today + 30 * 3600, today + 31 * 3600, FALSE);
    int values[] = { 900, 2340, 30, -540 };
    calendar_event_set_alerts (odd, values, 4);
    g_autofree char *odd_id = g_strdup (calendar_add (cal, odd));
    const CalEvent *shown = calendar_find (cal, odd_id);
    open_editor (shown, 0, 0, 0);
    CHECK (last_editor != NULL, "the editor opens for an event with unusual alerts");
    if (last_editor)
      {
        /* the two rows show the first two alerts by name, not as "None" */
        guint sel = adw_combo_row_get_selected (ADW_COMBO_ROW (last_editor->alert1));
        CHECK (sel < last_editor->alert_values->len && g_array_index (last_editor->alert_values, int, sel) == 900, "the first alert is shown as itself");
        sel = adw_combo_row_get_selected (ADW_COMBO_ROW (last_editor->alert2));
        CHECK (sel < last_editor->alert_values->len && g_array_index (last_editor->alert_values, int, sel) == 2340, "and the second");
        on_editor_save (NULL, last_editor);
      }
    const CalEvent *kept = calendar_find (cal, odd_id);
    CHECK (kept && calendar_event_alert_count (kept) == 4 && calendar_event_alert (kept, 0) == 900 && calendar_event_alert (kept, 1) == 2340 &&
           calendar_event_alert (kept, 2) == 30 && calendar_event_alert (kept, 3) == -540, "all four alerts survive a save untouched");
    g_autofree char *l1 = alert_label (900), *l2 = alert_label (-540), *l3 = alert_label (2340);
    CHECK (strstr (l1, "15") != NULL && strstr (l2, "9") != NULL && strstr (l3, "15") != NULL, "unusual alerts get readable names");
  }

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

/* for screenshots: open one of the dialogs on start */
void
calendar_ui_debug_open (const char *what)
{
  if (!U.view || !what)
    return;
  if (g_str_equal (what, "editor"))
    open_editor (NULL, 0, 0, 0);
  else if (g_str_equal (what, "editor-repeat"))
    {
      open_editor (NULL, 0, 0, 0);
      if (last_editor)
        {
          adw_combo_row_set_selected (ADW_COMBO_ROW (last_editor->repeat), CAL_REPEAT_WEEKLY);
          adw_combo_row_set_selected (ADW_COMBO_ROW (last_editor->ends_row), 2);
        }
    }
  else if (g_str_equal (what, "settings"))
    open_settings ();
  else if (g_str_equal (what, "goto"))
    open_goto ();
  else if (g_str_equal (what, "accounts"))
    calendar_accounts_open (U.view);
}

/* for tests: one PDF per view, without a dialog */
void
calendar_ui_debug_print (const char *directory)
{
  static const char *names[] = { "day", "week", "month", "year" };
  for (int v = 0; v < 4; v++)
    {
      g_autofree char *path = g_strdup_printf ("%s/%s.pdf", directory, names[v]);
      calendar_print (NULL, (PrintView) v, U.anchor, path);
    }
}

/* for tests: switches views and months many times, to see whether memory grows */
void
calendar_ui_stress (int rounds)
{
  for (int i = 0; i < rounds && U.view; i++)
    {
      for (int m = 0; m < 4; m++)
        {
          gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (U.toggle[m]), TRUE);
          on_step (NULL, GINT_TO_POINTER (i % 2 ? 1 : -1));
          for (int k = 0; k < 4; k++)
            g_main_context_iteration (NULL, FALSE);
        }
    }
}
