#pragma once

#include <glib.h>

/* Keeping calendars in step with a CalDAV server: iCloud (what iPhones use), Nextcloud,
 * Radicale and others. A calendar on the server becomes a local calendar; changes made
 * here are sent, changes made elsewhere are brought in. Only that server is contacted. */

typedef struct {
  char   *id;
  char   *name;       /* "iCloud" */
  char   *server;     /* https://caldav.icloud.com */
  char   *user;
  char   *password;   /* for iCloud, an app-specific password made at appleid.apple.com */
  char   *home;       /* where the account's calendars live, found on the first connection */
  gint64  last_sync;
  char   *last_error;
} CalAccount;

typedef struct {
  char *href;         /* absolute address */
  char *name;
  char *color;        /* "#RRGGBB" or NULL */
  char *source;       /* for an internet calendar you subscribed to on your phone: its address (https); else NULL */
} RemoteCalendar;

/* ---- accounts, kept in ~/.config/calendar/accounts.json (readable only by you) ---- */
const GPtrArray *caldav_accounts (void);                  /* CalAccount* */
CalAccount      *caldav_account_find (const char *id);
CalAccount      *caldav_account_add (const char *name, const char *server, const char *user, const char *password);
/* Forgets an account; its calendars stay here as local ones, or are deleted. */
void             caldav_account_remove (const char *id, gboolean delete_events);
void             caldav_accounts_save (void);
void             caldav_accounts_free (void);

/* Apple's app-specific passwords look like abcd-efgh-ijkl-mnop (spaces and case do not matter). */
gboolean caldav_looks_like_app_password (const char *password);
/* The server is iCloud. */
gboolean caldav_is_icloud (const char *server_url);

/* "https://…" always; "http://" only for this computer (testing). NULL when it is not acceptable. */
char *caldav_check_server_url (const char *text);

/* ---- finding the calendars an account has ---- */
typedef void (*DiscoverDone) (const char *error, GPtrArray *calendars, gpointer data);   /* RemoteCalendar*, freed after the call */
void caldav_discover (CalAccount *account, DiscoverDone done, gpointer data);
/* Makes a server calendar a local one that stays in step. Returns the local calendar's id. */
const char *caldav_link_calendar (CalAccount *account, const RemoteCalendar *remote);

/* ---- syncing ---- */
typedef void (*SyncDone) (const char *error, gpointer data);
/* Sends what changed here, brings what changed there. done may be NULL. */
void     caldav_sync_now (SyncDone done, gpointer data);
gboolean caldav_sync_running (void);
/* Syncs shortly after start, every 15 minutes, and a few seconds after each local change.
 * changed is called when events from the server arrived. */
void     caldav_sync_start (void (*changed) (void));
void     caldav_sync_stop (void);

/* Exposed for tests. */
char *caldav_resolve (const char *base, const char *href);

/* For tests: the text of the first element with this local name in an XML answer, or NULL. */
char *caldav_xml_first_text (const char *xml, const char *element);

/* For tests: the calendars named in a server's answer, and freeing such a list. */
GPtrArray *caldav_parse_calendar_list (const char *xml, const char *base);
void       caldav_remote_list_free (GPtrArray *list);
