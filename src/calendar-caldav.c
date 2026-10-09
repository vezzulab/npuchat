#include "calendar-caldav.h"

#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <string.h>

#include "calendar-ics.h"
#include "calendar.h"
#include "net.h"

#define SYNC_EVERY 900         /* seconds between syncs */
#define PUSH_DELAY 8           /* seconds after a local change before it is sent */
#define MULTIGET_BATCH 25
#define MAX_REDIRECTS 5

static GPtrArray *accounts;
static void (*changed_cb) (void);
static guint periodic, soon, first;
static gboolean running, again;
static GCancellable *cancel_all;

/* ---- a small XML tree --------------------------------------------------- */

typedef struct Xml {
  char        *name;      /* local name: prefixes are dropped, servers choose their own */
  GString     *text;
  GPtrArray   *kids;
  char       **attrs;     /* name, value, name, value, NULL */
} Xml;

typedef struct {
  GPtrArray *stack;
  Xml       *root;
} XmlBuild;

static void
xml_free (Xml *x)
{
  if (!x)
    return;
  g_free (x->name);
  g_string_free (x->text, TRUE);
  g_ptr_array_unref (x->kids);
  g_strfreev (x->attrs);
  g_free (x);
}

static const char *
local_name (const char *name)
{
  const char *colon = strrchr (name, ':');
  return colon ? colon + 1 : name;
}

static void
xml_start (GMarkupParseContext *ctx, const char *name, const char **names, const char **values, gpointer data, GError **error)
{
  (void) ctx; (void) error;
  XmlBuild *b = data;
  Xml *x = g_new0 (Xml, 1);
  x->name = g_strdup (local_name (name));
  x->text = g_string_new (NULL);
  x->kids = g_ptr_array_new_with_free_func ((GDestroyNotify) xml_free);
  GPtrArray *attrs = g_ptr_array_new ();
  for (int i = 0; names[i]; i++)
    {
      g_ptr_array_add (attrs, g_strdup (local_name (names[i])));
      g_ptr_array_add (attrs, g_strdup (values[i]));
    }
  g_ptr_array_add (attrs, NULL);
  x->attrs = (char **) g_ptr_array_free (attrs, FALSE);
  if (b->stack->len)
    g_ptr_array_add (((Xml *) b->stack->pdata[b->stack->len - 1])->kids, x);
  else
    b->root = x;
  g_ptr_array_add (b->stack, x);
}

static void
xml_end (GMarkupParseContext *ctx, const char *name, gpointer data, GError **error)
{
  (void) ctx; (void) name; (void) error;
  XmlBuild *b = data;
  g_ptr_array_remove_index (b->stack, b->stack->len - 1);
}

static void
xml_text (GMarkupParseContext *ctx, const char *text, gsize len, gpointer data, GError **error)
{
  (void) ctx; (void) error;
  XmlBuild *b = data;
  if (b->stack->len)
    g_string_append_len (((Xml *) b->stack->pdata[b->stack->len - 1])->text, text, (gssize) len);
}

/* NULL when the text is not well-formed XML */
static Xml *
xml_parse (const char *text)
{
  if (!text)
    return NULL;
  XmlBuild b = { g_ptr_array_new (), NULL };
  GMarkupParser parser = { xml_start, xml_end, xml_text, NULL, NULL };
  GMarkupParseContext *ctx = g_markup_parse_context_new (&parser, 0, &b, NULL);
  gboolean ok = g_markup_parse_context_parse (ctx, text, -1, NULL) && g_markup_parse_context_end_parse (ctx, NULL);
  g_markup_parse_context_free (ctx);
  g_ptr_array_unref (b.stack);
  if (!ok)
    {
      xml_free (b.root);
      return NULL;
    }
  return b.root;
}

/* the first descendant with this name */
static const Xml *
xml_find (const Xml *x, const char *name)
{
  if (!x)
    return NULL;
  for (guint i = 0; i < x->kids->len; i++)
    {
      const Xml *k = x->kids->pdata[i];
      if (g_str_equal (k->name, name))
        return k;
      const Xml *deep = xml_find (k, name);
      if (deep)
        return deep;
    }
  return NULL;
}

static const char *
xml_attr (const Xml *x, const char *name)
{
  for (int i = 0; x->attrs[i]; i += 2)
    if (g_str_equal (x->attrs[i], name))
      return x->attrs[i + 1];
  return NULL;
}

static char *
xml_text_of (const Xml *x)
{
  return x ? g_strstrip (g_strdup (x->text->str)) : NULL;
}

/* ---- addresses ---------------------------------------------------------- */

char *
caldav_resolve (const char *base, const char *href)
{
  if (!href || !*href)
    return NULL;
  g_autoptr (GError) error = NULL;
  char *r = g_uri_resolve_relative (base, href, G_URI_FLAGS_ENCODED, &error);
  return r;
}

char *
caldav_check_server_url (const char *text)
{
  if (!text)
    return NULL;
  g_autofree char *t = g_strstrip (g_strdup (text));
  if (!*t)
    return NULL;
  /* people type "caldav.icloud.com" */
  g_autofree char *with_scheme = strstr (t, "://") ? g_strdup (t) : g_strconcat ("https://", t, NULL);
  g_autoptr (GUri) uri = g_uri_parse (with_scheme, G_URI_FLAGS_PARSE_RELAXED, NULL);
  if (!uri || !g_uri_get_host (uri) || !*g_uri_get_host (uri))
    return NULL;
  const char *scheme = g_uri_get_scheme (uri), *host = g_uri_get_host (uri);
  if (g_str_equal (scheme, "https"))
    return g_steal_pointer (&with_scheme);
  /* passwords never travel in the clear, except to this computer */
  if (g_str_equal (scheme, "http") && (g_str_equal (host, "localhost") || g_str_equal (host, "127.0.0.1") || g_str_equal (host, "::1")))
    return g_steal_pointer (&with_scheme);
  return NULL;
}

/* ---- accounts ----------------------------------------------------------- */

static void
account_free (CalAccount *a)
{
  g_free (a->id);
  g_free (a->name);
  g_free (a->server);
  g_free (a->user);
  if (a->password)
    {
      memset (a->password, 0, strlen (a->password));    /* do not leave it lying in freed memory */
      g_free (a->password);
    }
  g_free (a->home);
  g_free (a->last_error);
  g_free (a);
}

static char *
accounts_path (void)
{
  const char *override = g_getenv ("CALENDAR_ACCOUNTS_FILE");     /* for tests */
  return override ? g_strdup (override) : g_build_filename (g_get_user_config_dir (), "calendar", "accounts.json", NULL);
}

static void
accounts_load (void)
{
  if (accounts)
    return;
  accounts = g_ptr_array_new_with_free_func ((GDestroyNotify) account_free);
  g_autofree char *path = accounts_path ();
  g_autoptr (JsonParser) parser = json_parser_new ();
  if (!json_parser_load_from_file (parser, path, NULL))
    return;
  JsonNode *root = json_parser_get_root (parser);
  if (!root || !JSON_NODE_HOLDS_ARRAY (root))
    return;
  JsonArray *list = json_node_get_array (root);
  for (guint i = 0; i < json_array_get_length (list); i++)
    {
      JsonNode *n = json_array_get_element (list, i);
      if (!JSON_NODE_HOLDS_OBJECT (n))
        continue;
      JsonObject *o = json_node_get_object (n);
      CalAccount *a = g_new0 (CalAccount, 1);
      a->id = g_strdup (json_object_get_string_member_with_default (o, "id", ""));
      a->name = g_strdup (json_object_get_string_member_with_default (o, "name", "CalDAV"));
      a->server = g_strdup (json_object_get_string_member_with_default (o, "server", ""));
      a->user = g_strdup (json_object_get_string_member_with_default (o, "user", ""));
      a->password = g_strdup (json_object_get_string_member_with_default (o, "password", ""));
      const char *home = json_object_get_string_member_with_default (o, "home", "");
      a->home = *home ? g_strdup (home) : NULL;
      a->last_sync = json_object_get_int_member_with_default (o, "last_sync", 0);
      if (!*a->id || !*a->server)
        {
          account_free (a);
          continue;
        }
      g_ptr_array_add (accounts, a);
    }
}

void
caldav_accounts_save (void)
{
  accounts_load ();
  g_autoptr (JsonBuilder) b = json_builder_new ();
  json_builder_begin_array (b);
  for (guint i = 0; i < accounts->len; i++)
    {
      const CalAccount *a = accounts->pdata[i];
      json_builder_begin_object (b);
      json_builder_set_member_name (b, "id");
      json_builder_add_string_value (b, a->id);
      json_builder_set_member_name (b, "name");
      json_builder_add_string_value (b, a->name);
      json_builder_set_member_name (b, "server");
      json_builder_add_string_value (b, a->server);
      json_builder_set_member_name (b, "user");
      json_builder_add_string_value (b, a->user);
      json_builder_set_member_name (b, "password");
      json_builder_add_string_value (b, a->password);
      json_builder_set_member_name (b, "home");
      json_builder_add_string_value (b, a->home ? a->home : "");
      json_builder_set_member_name (b, "last_sync");
      json_builder_add_int_value (b, a->last_sync);
      json_builder_end_object (b);
    }
  json_builder_end_array (b);
  g_autoptr (JsonGenerator) gen = json_generator_new ();
  g_autoptr (JsonNode) root = json_builder_get_root (b);
  json_generator_set_root (gen, root);
  json_generator_set_pretty (gen, TRUE);
  g_autofree char *text = json_generator_to_data (gen, NULL);
  g_autofree char *path = accounts_path ();
  g_autofree char *dir = g_path_get_dirname (path);
  g_mkdir_with_parents (dir, 0700);
  /* it holds a password: readable by you only, from the first byte */
  g_autoptr (GError) error = NULL;
  if (!g_file_set_contents_full (path, text, -1, G_FILE_SET_CONTENTS_CONSISTENT, 0600, &error))
    g_warning ("calendar: could not save the accounts: %s", error->message);
  memset (text, 0, strlen (text));
}

const GPtrArray *
caldav_accounts (void)
{
  accounts_load ();
  return accounts;
}

CalAccount *
caldav_account_find (const char *id)
{
  accounts_load ();
  for (guint i = 0; id && i < accounts->len; i++)
    if (g_str_equal (((CalAccount *) accounts->pdata[i])->id, id))
      return accounts->pdata[i];
  return NULL;
}

CalAccount *
caldav_account_add (const char *name, const char *server, const char *user, const char *password)
{
  accounts_load ();
  CalAccount *a = g_new0 (CalAccount, 1);
  a->id = g_uuid_string_random ();
  a->name = g_strdup (name);
  a->server = g_strdup (server);
  a->user = g_strdup (user);
  a->password = g_strdup (password);
  g_ptr_array_add (accounts, a);
  caldav_accounts_save ();
  return a;
}

void
caldav_account_remove (const char *id, gboolean delete_events)
{
  accounts_load ();
  calendar_unlink_account (calendar_default (), id, delete_events);
  for (guint i = 0; i < accounts->len; i++)
    if (g_str_equal (((CalAccount *) accounts->pdata[i])->id, id))
      {
        g_ptr_array_remove_index (accounts, i);
        break;
      }
  caldav_accounts_save ();
}

void
caldav_accounts_free (void)
{
  g_clear_pointer (&accounts, g_ptr_array_unref);
}

/* ---- one HTTP request --------------------------------------------------- */

typedef struct {
  guint  status;
  char  *etag;
  char  *location;
  char  *body;
} Reply;

typedef void (*ReplyCb) (const Reply *r, const char *error, gpointer data);

typedef struct {
  char        *method, *url, *depth, *content_type, *body, *if_match;
  gboolean     if_none_match;
  char        *auth;
  int          redirects;
  ReplyCb      cb;
  gpointer     data;
  SoupMessage *msg;
} Request;

static void request_send (Request *rq);

static void
request_free (Request *rq)
{
  g_free (rq->method);
  g_free (rq->url);
  g_free (rq->depth);
  g_free (rq->content_type);
  g_free (rq->body);
  g_free (rq->if_match);
  if (rq->auth)
    {
      memset (rq->auth, 0, strlen (rq->auth));
      g_free (rq->auth);
    }
  g_clear_object (&rq->msg);
  g_free (rq);
}

static void
on_reply (GObject *source, GAsyncResult *res, gpointer data)
{
  Request *rq = data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) body = soup_session_send_and_read_finish (SOUP_SESSION (source), res, &error);
  if (!body)
    {
      rq->cb (NULL, error->message, rq->data);
      request_free (rq);
      return;
    }
  Reply r = { soup_message_get_status (rq->msg), NULL, NULL, NULL };
  SoupMessageHeaders *h = soup_message_get_response_headers (rq->msg);
  const char *etag = soup_message_headers_get_one (h, "ETag");
  const char *loc = soup_message_headers_get_one (h, "Location");
  r.etag = g_strdup (etag);
  r.location = g_strdup (loc);
  gsize len = 0;
  const char *data_ptr = g_bytes_get_data (body, &len);
  r.body = data_ptr && len ? g_strndup (data_ptr, len) : g_strdup ("");
  if ((r.status == 301 || r.status == 302 || r.status == 307 || r.status == 308) && r.location && rq->redirects < MAX_REDIRECTS)
    {
      /* followed by hand, so the method and the login travel with it */
      g_autofree char *next = caldav_resolve (rq->url, r.location);
      if (next)
        {
          g_free (rq->url);
          rq->url = g_steal_pointer (&next);
          rq->redirects++;
          g_free (r.etag);
          g_free (r.location);
          g_free (r.body);
          request_send (rq);
          return;
        }
    }
  rq->cb (&r, NULL, rq->data);
  g_free (r.etag);
  g_free (r.location);
  g_free (r.body);
  request_free (rq);
}

static void
request_send (Request *rq)
{
  g_clear_object (&rq->msg);
  rq->msg = soup_message_new (rq->method, rq->url);
  if (!rq->msg)
    {
      rq->cb (NULL, "That address is not valid.", rq->data);
      request_free (rq);
      return;
    }
  soup_message_add_flags (rq->msg, SOUP_MESSAGE_NO_REDIRECT);
  SoupMessageHeaders *h = soup_message_get_request_headers (rq->msg);
  soup_message_headers_replace (h, "Authorization", rq->auth);
  if (rq->depth)
    soup_message_headers_replace (h, "Depth", rq->depth);
  if (rq->if_match)
    soup_message_headers_replace (h, "If-Match", rq->if_match);
  if (rq->if_none_match)
    soup_message_headers_replace (h, "If-None-Match", "*");
  if (rq->body)
    {
      g_autoptr (GBytes) bytes = g_bytes_new (rq->body, strlen (rq->body));
      soup_message_set_request_body_from_bytes (rq->msg, rq->content_type, bytes);
    }
  if (!cancel_all)
    cancel_all = g_cancellable_new ();
  soup_session_send_and_read_async (net_session (), rq->msg, G_PRIORITY_DEFAULT, cancel_all, on_reply, rq);
}

static void
http (const CalAccount *a, const char *method, const char *url, const char *depth, const char *content_type, const char *body,
      const char *if_match, gboolean if_none_match, ReplyCb cb, gpointer data)
{
  Request *rq = g_new0 (Request, 1);
  rq->method = g_strdup (method);
  rq->url = g_strdup (url);
  rq->depth = g_strdup (depth);
  rq->content_type = g_strdup (content_type);
  rq->body = g_strdup (body);
  rq->if_match = g_strdup (if_match);
  rq->if_none_match = if_none_match;
  g_autofree char *login = g_strdup_printf ("%s:%s", a->user, a->password);
  g_autofree char *encoded = g_base64_encode ((const guchar *) login, strlen (login));
  rq->auth = g_strdup_printf ("Basic %s", encoded);
  memset (login, 0, strlen (login));
  rq->cb = cb;
  rq->data = data;
  request_send (rq);
}

/* ---- discovery ---------------------------------------------------------- */

typedef struct {
  CalAccount  *account;       /* looked up again on every step: it may be removed meanwhile */
  char        *account_id;
  DiscoverDone done;
  gpointer     data;
  char        *base;          /* the address the current answer came from */
  gboolean     tried_well_known;
} Discover;

static void discover_step_home (Discover *d, const char *principal);

static void
discover_finish (Discover *d, const char *error, GPtrArray *cals)
{
  GPtrArray *list = cals ? cals : g_ptr_array_new ();
  d->done (error, list, d->data);
  for (guint i = 0; i < list->len; i++)
    {
      RemoteCalendar *c = list->pdata[i];
      g_free (c->href);
      g_free (c->name);
      g_free (c->color);
      g_free (c);
    }
  g_ptr_array_unref (list);
  g_free (d->account_id);
  g_free (d->base);
  g_free (d);
}

static const char *
describe_status (guint status)
{
  switch (status)
    {
    case 401: return "The user name or password was not accepted.";
    case 403: return "The server refused the request.";
    case 404: return "The server does not have that address.";
    case 429: return "The server asks to try again later.";
    default:  return "The server did not answer as a calendar server should.";
    }
}

static GPtrArray *
parse_calendars (const Xml *root, const char *base)
{
  GPtrArray *out = g_ptr_array_new ();
  for (guint i = 0; root && i < root->kids->len; i++)
    {
      const Xml *resp = root->kids->pdata[i];
      if (!g_str_equal (resp->name, "response"))
        continue;
      for (guint k = 0; k < resp->kids->len; k++)
        {
          const Xml *ps = resp->kids->pdata[k];
          if (!g_str_equal (ps->name, "propstat"))
            continue;
          g_autofree char *status = xml_text_of (xml_find (ps, "status"));
          if (!status || !strstr (status, " 200"))
            continue;
          const Xml *prop = xml_find (ps, "prop");
          const Xml *type = xml_find (prop, "resourcetype");
          if (!type || !xml_find (type, "calendar"))
            continue;
          const Xml *comps = xml_find (prop, "supported-calendar-component-set");
          gboolean events = !comps;
          for (guint c = 0; comps && c < comps->kids->len; c++)
            {
              const char *n = xml_attr (comps->kids->pdata[c], "name");
              events |= n && g_ascii_strcasecmp (n, "VEVENT") == 0;
            }
          if (!events)
            continue;      /* a list of to-dos, not events */
          g_autofree char *href = xml_text_of (xml_find (resp, "href"));
          g_autofree char *abs = caldav_resolve (base, href);
          if (!abs)
            continue;
          RemoteCalendar *rc = g_new0 (RemoteCalendar, 1);
          rc->href = g_steal_pointer (&abs);
          g_autofree char *name = xml_text_of (xml_find (prop, "displayname"));
          rc->name = name && *name ? g_steal_pointer (&name) : g_strdup ("Calendar");
          g_autofree char *color = xml_text_of (xml_find (prop, "calendar-color"));
          rc->color = color && strlen (color) >= 7 && color[0] == '#' ? g_strndup (color, 7) : NULL;
          g_ptr_array_add (out, rc);
        }
    }
  return out;
}

static void
on_calendars (const Reply *r, const char *error, gpointer data)
{
  Discover *d = data;
  if (error)
    {
      discover_finish (d, error, NULL);
      return;
    }
  if (r->status != 207)
    {
      discover_finish (d, describe_status (r->status), NULL);
      return;
    }
  g_autoptr (GPtrArray) none = NULL;
  Xml *root = xml_parse (r->body);
  if (!root)
    {
      discover_finish (d, "The server's answer could not be read.", NULL);
      return;
    }
  GPtrArray *cals = parse_calendars (root, d->base);
  xml_free (root);
  discover_finish (d, NULL, cals);
}

static void
on_home (const Reply *r, const char *error, gpointer data)
{
  Discover *d = data;
  CalAccount *a = caldav_account_find (d->account_id);
  if (error || !a)
    {
      discover_finish (d, error ? error : "The account was removed.", NULL);
      return;
    }
  if (r->status != 207)
    {
      discover_finish (d, describe_status (r->status), NULL);
      return;
    }
  Xml *root = xml_parse (r->body);
  g_autofree char *home_href = root ? xml_text_of (xml_find (xml_find (root, "calendar-home-set"), "href")) : NULL;
  xml_free (root);
  g_autofree char *home = home_href ? caldav_resolve (d->base, home_href) : NULL;
  if (!home)
    {
      discover_finish (d, "The server did not say where the calendars are.", NULL);
      return;
    }
  g_free (a->home);
  a->home = g_strdup (home);
  g_free (d->base);
  d->base = g_strdup (home);
  static const char *body =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<d:propfind xmlns:d=\"DAV:\" xmlns:c=\"urn:ietf:params:xml:ns:caldav\" xmlns:a=\"http://apple.com/ns/ical/\">"
    "<d:prop><d:displayname/><d:resourcetype/><c:supported-calendar-component-set/><a:calendar-color/></d:prop></d:propfind>";
  http (a, "PROPFIND", home, "1", "application/xml; charset=utf-8", body, NULL, FALSE, on_calendars, d);
}

static void
discover_step_home (Discover *d, const char *principal)
{
  CalAccount *a = caldav_account_find (d->account_id);
  if (!a)
    {
      discover_finish (d, "The account was removed.", NULL);
      return;
    }
  static const char *body =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<d:propfind xmlns:d=\"DAV:\" xmlns:c=\"urn:ietf:params:xml:ns:caldav\"><d:prop><c:calendar-home-set/></d:prop></d:propfind>";
  g_free (d->base);
  d->base = g_strdup (principal);
  http (a, "PROPFIND", principal, "0", "application/xml; charset=utf-8", body, NULL, FALSE, on_home, d);
}

static void
on_principal (const Reply *r, const char *error, gpointer data)
{
  Discover *d = data;
  CalAccount *a = caldav_account_find (d->account_id);
  if (error || !a)
    {
      discover_finish (d, error ? error : "The account was removed.", NULL);
      return;
    }
  if (r->status != 207)
    {
      /* RFC 6764: servers publish the way in at /.well-known/caldav */
      if ((r->status == 404 || r->status == 405) && !d->tried_well_known)
        {
          d->tried_well_known = TRUE;
          g_autofree char *wk = caldav_resolve (a->server, "/.well-known/caldav");
          static const char *body =
            "<?xml version=\"1.0\" encoding=\"utf-8\"?><d:propfind xmlns:d=\"DAV:\"><d:prop><d:current-user-principal/></d:prop></d:propfind>";
          g_free (d->base);
          d->base = g_strdup (wk);
          http (a, "PROPFIND", wk, "0", "application/xml; charset=utf-8", body, NULL, FALSE, on_principal, d);
          return;
        }
      discover_finish (d, describe_status (r->status), NULL);
      return;
    }
  Xml *root = xml_parse (r->body);
  g_autofree char *href = root ? xml_text_of (xml_find (xml_find (root, "current-user-principal"), "href")) : NULL;
  xml_free (root);
  g_autofree char *principal = href ? caldav_resolve (d->base, href) : NULL;
  if (!principal)
    {
      discover_finish (d, "The server did not say who you are.", NULL);
      return;
    }
  discover_step_home (d, principal);
}

void
caldav_discover (CalAccount *account, DiscoverDone done, gpointer data)
{
  Discover *d = g_new0 (Discover, 1);
  d->account_id = g_strdup (account->id);
  d->done = done;
  d->data = data;
  d->base = g_strdup (account->server);
  static const char *body =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?><d:propfind xmlns:d=\"DAV:\"><d:prop><d:current-user-principal/></d:prop></d:propfind>";
  http (account, "PROPFIND", account->server, "0", "application/xml; charset=utf-8", body, NULL, FALSE, on_principal, d);
}

const char *
caldav_link_calendar (CalAccount *account, const RemoteCalendar *remote)
{
  guint color = remote->color ? calendar_color_nearest (remote->color) : calendar_calendars (calendar_default ())->len;
  return calendar_linked_add (calendar_default (), remote->name, color, account->id, remote->href);
}

/* ---- syncing ------------------------------------------------------------ */

typedef struct {
  char *account_id;
  char *calendar_id;
} Job;

typedef struct {
  GPtrArray  *jobs;
  guint       ji;
  SyncDone    done;
  gpointer    data;
  char       *error;
  gboolean    arrived;      /* something came from the server */
  /* the calendar being synced */
  char       *account_id, *cal_id, *cal_href, *cal_name;
  GPtrArray  *push_uids;
  guint       pi;
  GHashTable *conflicts;    /* uid -> 1: the server's version wins */
  GHashTable *server;       /* absolute href -> etag */
  GPtrArray  *fetch;
  guint       fi;
} Run;

static void run_next_job (Run *run);
static void push_next (Run *run);
static void phase_delete (Run *run);
static void phase_pull (Run *run);
static void fetch_next (Run *run);

static void
job_free (Job *j)
{
  g_free (j->account_id);
  g_free (j->calendar_id);
  g_free (j);
}

static void
run_note_error (Run *run, const char *message)
{
  if (!run->error)
    run->error = g_strdup_printf ("%s: %s", run->cal_name ? run->cal_name : "?", message);
}

static void
run_finish (Run *run)
{
  running = FALSE;
  gint64 now = g_get_real_time () / G_USEC_PER_SEC;
  for (guint i = 0; i < run->jobs->len; i++)
    {
      Job *j = run->jobs->pdata[i];
      CalAccount *a = caldav_account_find (j->account_id);
      if (a)
        {
          a->last_sync = now;
          g_free (a->last_error);
          a->last_error = g_strdup (run->error);
        }
    }
  caldav_accounts_save ();
  if (run->arrived && changed_cb)
    changed_cb ();
  SyncDone done = run->done;
  gpointer data = run->data;
  g_autofree char *error = g_strdup (run->error);
  g_ptr_array_unref (run->jobs);
  g_free (run->error);
  g_free (run->account_id);
  g_free (run->cal_id);
  g_free (run->cal_href);
  g_free (run->cal_name);
  g_clear_pointer (&run->push_uids, g_ptr_array_unref);
  g_clear_pointer (&run->conflicts, g_hash_table_unref);
  g_clear_pointer (&run->server, g_hash_table_unref);
  g_clear_pointer (&run->fetch, g_ptr_array_unref);
  g_free (run);
  if (done)
    done (error, data);
  if (again)
    {
      again = FALSE;
      caldav_sync_now (NULL, NULL);
    }
}

static void
calendar_complete (Run *run)
{
  CalCalendar *c = calendar_calendar_find (calendar_default (), run->cal_id);
  if (c && g_str_equal (c->id, run->cal_id))
    c->fetched = g_get_real_time () / G_USEC_PER_SEC;
  run->ji++;
  run_next_job (run);
}

static void
calendar_failed (Run *run, const char *message)
{
  run_note_error (run, message);
  run->ji++;
  run_next_job (run);
}

static const char *
reason (const Reply *r, const char *error)
{
  return error ? error : describe_status (r->status);
}

/* ---- deletions the server has not heard of ---- */

static void
on_deleted (const Reply *r, const char *error, gpointer data)
{
  Run *run = data;
  if (error)
    {
      calendar_failed (run, error);
      return;
    }
  /* gone, already gone, or changed there meanwhile: either way there is nothing left to send */
  if ((r->status >= 200 && r->status < 300) || r->status == 404 || r->status == 410 || r->status == 412)
    {
      const GPtrArray *pending = calendar_pending_deletes (calendar_default ());
      for (guint i = 0; i < pending->len; i++)
        {
          CalPendingDelete *p = pending->pdata[i];
          if (g_str_equal (p->calendar, run->cal_id))
            {
              g_autofree char *href = g_strdup (p->href);
              calendar_clear_pending_delete (calendar_default (), href);
              break;
            }
        }
      phase_delete (run);
      return;
    }
  calendar_failed (run, describe_status (r->status));
}

static void
phase_delete (Run *run)
{
  CalAccount *a = caldav_account_find (run->account_id);
  const GPtrArray *pending = calendar_pending_deletes (calendar_default ());
  for (guint i = 0; a && i < pending->len; i++)
    {
      CalPendingDelete *p = pending->pdata[i];
      if (g_str_equal (p->calendar, run->cal_id))
        {
          http (a, "DELETE", p->href, NULL, NULL, NULL, p->etag, FALSE, on_deleted, run);
          return;
        }
    }
  /* then what changed here */
  g_autoptr (GPtrArray) dirty = calendar_dirty_events (calendar_default (), run->cal_id);
  run->push_uids = g_ptr_array_new_with_free_func (g_free);
  for (guint i = 0; i < dirty->len; i++)
    {
      const CalEvent *e = dirty->pdata[i];
      gboolean seen = FALSE;
      for (guint k = 0; k < run->push_uids->len; k++)
        seen |= e->uid && g_str_equal (run->push_uids->pdata[k], e->uid);
      if (e->uid && !seen)
        g_ptr_array_add (run->push_uids, g_strdup (e->uid));
    }
  run->pi = 0;
  push_next (run);
}

/* ---- what changed here ---- */

typedef struct {
  Run   *run;
  char  *uid;
  char  *href;
  gboolean created;     /* sent with If-None-Match */
  gboolean retried;
} Push;

static void
push_free (Push *p)
{
  g_free (p->uid);
  g_free (p->href);
  g_free (p);
}

static char *
new_resource_href (const char *calendar_href, const char *uid)
{
  GString *name = g_string_new (NULL);
  for (const char *c = uid; *c; c++)
    g_string_append_c (name, g_ascii_isalnum (*c) || *c == '-' || *c == '_' || *c == '.' ? *c : '_');
  g_string_append (name, ".ics");
  g_autofree char *base = g_str_has_suffix (calendar_href, "/") ? g_strdup (calendar_href) : g_strconcat (calendar_href, "/", NULL);
  char *href = g_strconcat (base, name->str, NULL);
  g_string_free (name, TRUE);
  return href;
}

static void send_push (Push *p, gboolean create);

static void
on_pushed (const Reply *r, const char *error, gpointer data)
{
  Push *p = data;
  Run *run = p->run;
  if (error)
    {
      calendar_failed (run, error);
      push_free (p);
      return;
    }
  if (r->status >= 200 && r->status < 300)
    {
      calendar_mark_synced (calendar_default (), run->cal_id, p->uid, p->href, r->etag);
      run->pi++;
      push_free (p);
      push_next (run);
      return;
    }
  if (r->status == 404 || r->status == 410)
    {
      /* it vanished from the server: send it again as a new resource, once */
      if (!p->created && !p->retried)
        {
          p->retried = TRUE;
          send_push (p, TRUE);
          return;
        }
    }
  if (r->status == 412 || r->status == 409)
    {
      /* someone changed it there too: their version wins, and arrives with the pull */
      g_hash_table_add (run->conflicts, g_strdup (p->uid));
      run->pi++;
      push_free (p);
      push_next (run);
      return;
    }
  calendar_failed (run, describe_status (r->status));
  push_free (p);
}

static void
send_push (Push *p, gboolean create)
{
  Run *run = p->run;
  CalAccount *a = caldav_account_find (run->account_id);
  g_autoptr (GPtrArray) group = calendar_events_with_uid (calendar_default (), run->cal_id, p->uid);
  if (!a || !group->len)
    {
      run->pi++;
      push_free (p);
      push_next (run);
      return;
    }
  const char *etag = NULL;
  g_free (p->href);
  p->href = NULL;
  for (guint i = 0; i < group->len; i++)
    {
      const CalEvent *e = group->pdata[i];
      if (e->href && !create && !p->href)
        {
          p->href = g_strdup (e->href);
          etag = e->etag;
        }
    }
  p->created = create || !p->href;
  if (p->created)
    {
      g_free (p->href);
      p->href = new_resource_href (run->cal_href, p->uid);
      etag = NULL;
    }
  g_autofree char *ics = calendar_ics_export_events (group);
  http (a, "PUT", p->href, NULL, "text/calendar; charset=utf-8", ics, etag, p->created, on_pushed, p);
}

static void
push_next (Run *run)
{
  if (run->pi >= run->push_uids->len)
    {
      phase_pull (run);
      return;
    }
  Push *p = g_new0 (Push, 1);
  p->run = run;
  p->uid = g_strdup (run->push_uids->pdata[run->pi]);
  send_push (p, FALSE);
}

/* ---- what changed there ---- */

static gboolean
local_dirty_at (const char *href, const char *calendar_id, const GHashTable *conflicts)
{
  const GPtrArray *events = calendar_all_events (calendar_default ());
  for (guint i = 0; i < events->len; i++)
    {
      const CalEvent *e = events->pdata[i];
      if (e->href && g_str_equal (e->href, href) && g_str_equal (e->calendar, calendar_id) && e->dirty &&
          !(e->uid && g_hash_table_contains ((GHashTable *) conflicts, e->uid)))
        return TRUE;
    }
  return FALSE;
}

static const char *
local_etag_at (const char *href, const char *calendar_id)
{
  const GPtrArray *events = calendar_all_events (calendar_default ());
  for (guint i = 0; i < events->len; i++)
    {
      const CalEvent *e = events->pdata[i];
      if (e->href && g_str_equal (e->href, href) && g_str_equal (e->calendar, calendar_id))
        return e->etag ? e->etag : "";
    }
  return NULL;
}

static void
on_server_list (const Reply *r, const char *error, gpointer data)
{
  Run *run = data;
  if (error)
    {
      calendar_failed (run, error);
      return;
    }
  if (r->status != 207)
    {
      calendar_failed (run, reason (r, NULL));
      return;
    }
  Xml *root = xml_parse (r->body);
  if (!root)
    {
      calendar_failed (run, "The server's answer could not be read.");
      return;
    }
  run->server = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  g_autofree char *self = g_strdup (run->cal_href);
  g_strchomp (self);
  for (guint i = 0; i < root->kids->len; i++)
    {
      const Xml *resp = root->kids->pdata[i];
      if (!g_str_equal (resp->name, "response"))
        continue;
      g_autofree char *href = xml_text_of (xml_find (resp, "href"));
      g_autofree char *abs = caldav_resolve (run->cal_href, href);
      if (!abs)
        continue;
      const Xml *type = xml_find (resp, "resourcetype");
      gboolean collection = type && xml_find (type, "collection");
      g_autofree char *a_noslash = g_strdup (abs);
      g_autofree char *c_noslash = g_strdup (self);
      if (g_str_has_suffix (a_noslash, "/"))
        a_noslash[strlen (a_noslash) - 1] = 0;
      if (g_str_has_suffix (c_noslash, "/"))
        c_noslash[strlen (c_noslash) - 1] = 0;
      if (collection || g_str_equal (a_noslash, c_noslash))
        continue;
      g_autofree char *etag = xml_text_of (xml_find (resp, "getetag"));
      g_hash_table_insert (run->server, g_steal_pointer (&abs), g_strdup (etag ? etag : ""));
    }
  xml_free (root);

  /* what to fetch: new on the server, or a different version than ours */
  run->fetch = g_ptr_array_new_with_free_func (g_free);
  GHashTableIter it;
  gpointer k, v;
  g_hash_table_iter_init (&it, run->server);
  while (g_hash_table_iter_next (&it, &k, &v))
    {
      const char *mine = local_etag_at (k, run->cal_id);
      if (mine && g_str_equal (mine, (const char *) v) && *mine)
        continue;
      if (local_dirty_at (k, run->cal_id, run->conflicts))
        continue;                       /* ours is newer; it goes out on the next round */
      g_ptr_array_add (run->fetch, g_strdup (k));
    }

  /* gone from the server, and not changed here: gone here too */
  g_autoptr (GPtrArray) stale = g_ptr_array_new_with_free_func (g_free);
  const GPtrArray *events = calendar_all_events (calendar_default ());
  for (guint i = 0; i < events->len; i++)
    {
      const CalEvent *e = events->pdata[i];
      if (!e->href || !g_str_equal (e->calendar, run->cal_id) || e->dirty || g_hash_table_contains (run->server, e->href))
        continue;
      gboolean seen = FALSE;
      for (guint s = 0; s < stale->len; s++)
        seen |= g_str_equal (stale->pdata[s], e->href);
      if (!seen)
        g_ptr_array_add (stale, g_strdup (e->href));
    }
  for (guint i = 0; i < stale->len; i++)
    {
      calendar_remove_resource (calendar_default (), run->cal_id, stale->pdata[i]);
      run->arrived = TRUE;
    }
  run->fi = 0;
  fetch_next (run);
}

static void
phase_pull (Run *run)
{
  CalAccount *a = caldav_account_find (run->account_id);
  if (!a)
    {
      calendar_failed (run, "The account was removed.");
      return;
    }
  static const char *body =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<d:propfind xmlns:d=\"DAV:\"><d:prop><d:getetag/><d:resourcetype/></d:prop></d:propfind>";
  http (a, "PROPFIND", run->cal_href, "1", "application/xml; charset=utf-8", body, NULL, FALSE, on_server_list, run);
}

static void
on_multiget (const Reply *r, const char *error, gpointer data)
{
  Run *run = data;
  if (error)
    {
      calendar_failed (run, error);
      return;
    }
  if (r->status != 207)
    {
      calendar_failed (run, reason (r, NULL));
      return;
    }
  Xml *root = xml_parse (r->body);
  if (!root)
    {
      calendar_failed (run, "The server's answer could not be read.");
      return;
    }
  for (guint i = 0; i < root->kids->len; i++)
    {
      const Xml *resp = root->kids->pdata[i];
      if (!g_str_equal (resp->name, "response"))
        continue;
      g_autofree char *href = xml_text_of (xml_find (resp, "href"));
      g_autofree char *abs = caldav_resolve (run->cal_href, href);
      const Xml *data_node = xml_find (resp, "calendar-data");
      if (!abs || !data_node || !data_node->text->len)
        continue;
      g_autofree char *etag = xml_text_of (xml_find (resp, "getetag"));
      GPtrArray *events = calendar_ics_parse (data_node->text->str);
      for (guint e = 0; e < events->len; e++)
        {
          CalEvent *ev = events->pdata[e];
          g_free (ev->etag);
          ev->etag = etag && *etag ? g_strdup (etag) : NULL;
        }
      calendar_replace_resource (calendar_default (), run->cal_id, abs, events);
      g_ptr_array_unref (events);
      run->arrived = TRUE;
    }
  xml_free (root);
  fetch_next (run);
}

static void
fetch_next (Run *run)
{
  CalAccount *a = caldav_account_find (run->account_id);
  if (!a)
    {
      calendar_failed (run, "The account was removed.");
      return;
    }
  if (run->fi >= run->fetch->len)
    {
      calendar_complete (run);
      return;
    }
  GString *body = g_string_new ("<?xml version=\"1.0\" encoding=\"utf-8\"?>"
                                "<c:calendar-multiget xmlns:d=\"DAV:\" xmlns:c=\"urn:ietf:params:xml:ns:caldav\">"
                                "<d:prop><d:getetag/><c:calendar-data/></d:prop>");
  guint end = MIN (run->fi + MULTIGET_BATCH, run->fetch->len);
  for (guint i = run->fi; i < end; i++)
    {
      g_autoptr (GUri) uri = g_uri_parse (run->fetch->pdata[i], G_URI_FLAGS_ENCODED, NULL);
      g_autofree char *path = g_markup_escape_text (uri ? g_uri_get_path (uri) : (char *) run->fetch->pdata[i], -1);
      g_string_append_printf (body, "<d:href>%s</d:href>", path);
    }
  g_string_append (body, "</c:calendar-multiget>");
  run->fi = end;
  http (a, "REPORT", run->cal_href, "1", "application/xml; charset=utf-8", body->str, NULL, FALSE, on_multiget, run);
  g_string_free (body, TRUE);
}

/* ---- the run ------------------------------------------------------------ */

static void
run_next_job (Run *run)
{
  while (run->ji < run->jobs->len)
    {
      Job *j = run->jobs->pdata[run->ji];
      CalAccount *a = caldav_account_find (j->account_id);
      CalCalendar *c = calendar_calendar_find (calendar_default (), j->calendar_id);
      if (!a || !c || !g_str_equal (c->id, j->calendar_id) || !c->href)
        {
          run->ji++;
          continue;
        }
      g_free (run->account_id);
      g_free (run->cal_id);
      g_free (run->cal_href);
      g_free (run->cal_name);
      run->account_id = g_strdup (j->account_id);
      run->cal_id = g_strdup (j->calendar_id);
      run->cal_href = g_strdup (c->href);
      run->cal_name = g_strdup (c->name);
      g_clear_pointer (&run->push_uids, g_ptr_array_unref);
      g_clear_pointer (&run->conflicts, g_hash_table_unref);
      g_clear_pointer (&run->server, g_hash_table_unref);
      g_clear_pointer (&run->fetch, g_ptr_array_unref);
      run->conflicts = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
      phase_delete (run);
      return;
    }
  run_finish (run);
}

gboolean
caldav_sync_running (void)
{
  return running;
}

void
caldav_sync_now (SyncDone done, gpointer data)
{
  if (running)
    {
      again = TRUE;
      if (done)
        done (NULL, data);
      return;
    }
  Run *run = g_new0 (Run, 1);
  run->jobs = g_ptr_array_new_with_free_func ((GDestroyNotify) job_free);
  run->done = done;
  run->data = data;
  GPtrArray *cals = calendar_calendars (calendar_default ());
  for (guint i = 0; i < cals->len; i++)
    {
      const CalCalendar *c = cals->pdata[i];
      if (c->account && c->href && caldav_account_find (c->account))
        {
          Job *j = g_new0 (Job, 1);
          j->account_id = g_strdup (c->account);
          j->calendar_id = g_strdup (c->id);
          g_ptr_array_add (run->jobs, j);
        }
    }
  if (!run->jobs->len)
    {
      g_ptr_array_unref (run->jobs);
      g_free (run);
      if (done)
        done (NULL, data);
      return;
    }
  running = TRUE;
  run_next_job (run);
}

/* ---- the schedule ------------------------------------------------------- */

static gboolean
has_work (void)
{
  if (calendar_pending_deletes (calendar_default ())->len)
    return TRUE;
  const GPtrArray *events = calendar_all_events (calendar_default ());
  for (guint i = 0; i < events->len; i++)
    if (((CalEvent *) events->pdata[i])->dirty)
      return TRUE;
  return FALSE;
}

static gboolean
on_soon (gpointer data)
{
  (void) data;
  soon = 0;
  caldav_sync_now (NULL, NULL);
  return G_SOURCE_REMOVE;
}

/* every save lands here; only unsent changes are worth a trip to the server */
static void
on_local_change (void)
{
  if (soon || !has_work ())
    return;
  soon = g_timeout_add_seconds (PUSH_DELAY, on_soon, NULL);
}

static gboolean
on_periodic (gpointer data)
{
  (void) data;
  caldav_sync_now (NULL, NULL);
  return G_SOURCE_CONTINUE;
}

static gboolean
on_first (gpointer data)
{
  (void) data;
  first = 0;
  caldav_sync_now (NULL, NULL);
  return G_SOURCE_REMOVE;
}

void
caldav_sync_start (void (*changed) (void))
{
  changed_cb = changed;
  calendar_set_change_hook (on_local_change);
  if (!first)
    first = g_timeout_add_seconds (6, on_first, NULL);
  if (!periodic)
    periodic = g_timeout_add_seconds (SYNC_EVERY, on_periodic, NULL);
}

void
caldav_sync_stop (void)
{
  calendar_set_change_hook (NULL);
  if (first)
    g_source_remove (first);
  if (periodic)
    g_source_remove (periodic);
  if (soon)
    g_source_remove (soon);
  first = periodic = soon = 0;
  if (cancel_all)
    g_cancellable_cancel (cancel_all);
  /* let the cancelled requests report back before anything they use goes away */
  for (int i = 0; running && i < 2000; i++)
    g_main_context_iteration (NULL, TRUE);
  g_clear_object (&cancel_all);
  changed_cb = NULL;
  caldav_accounts_free ();
}
