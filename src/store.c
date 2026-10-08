#include "store.h"

#include <glib/gstdio.h>
#include <json-glib/json-glib.h>

static void
store_msg_free (gpointer p)
{
  StoreMsg *m = p;
  g_free (m->role);
  g_free (m->content);
  g_free (m->think);
  g_free (m->stats);
  g_free (m->author);
  g_free (m->author_emoji);
  if (m->sources)
    g_ptr_array_unref (m->sources);
  if (m->tools)
    g_ptr_array_unref (m->tools);
  g_free (m->note);
  g_free (m);
}

static void
store_source_free (gpointer p)
{
  StoreSource *s = p;
  g_free (s->file);
  g_free (s->text);
  g_free (s);
}

static void
store_tool_free (gpointer p)
{
  StoreTool *t = p;
  g_free (t->name);
  g_free (t->args);
  g_free (t->result);
  g_free (t);
}

TeamMember *
team_member_new (const char *id, const char *name, const char *emoji, const char *instructions)
{
  TeamMember *t = g_new0 (TeamMember, 1);
  t->id = g_strdup (id);
  t->name = g_strdup (name);
  t->emoji = g_strdup (emoji);
  t->instructions = g_strdup (instructions ? instructions : "");
  return t;
}

void
team_member_free (gpointer p)
{
  TeamMember *t = p;
  g_free (t->id);
  g_free (t->name);
  g_free (t->emoji);
  g_free (t->instructions);
  g_free (t);
}

Conversation *
conversation_new (void)
{
  Conversation *c = g_new0 (Conversation, 1);
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  g_autofree char *stamp = g_date_time_format (now, "%Y%m%d-%H%M%S");

  c->id = g_strdup_printf ("%s-%04x", stamp, g_random_int_range (0, 0xffff));
  c->updated = g_date_time_to_unix (now);
  c->msgs = g_ptr_array_new_with_free_func (store_msg_free);
  c->team = g_ptr_array_new_with_free_func (team_member_free);
  c->libs = g_ptr_array_new_with_free_func (g_free);
  c->loaded = TRUE;
  return c;
}

void
conversation_free (Conversation *c)
{
  if (!c)
    return;
  g_free (c->id);
  g_free (c->title);
  g_free (c->model);
  g_free (c->assistant_id);
  g_free (c->assistant_name);
  g_free (c->assistant_emoji);
  g_free (c->system);
  g_ptr_array_unref (c->team);
  g_ptr_array_unref (c->libs);
  g_ptr_array_unref (c->msgs);
  g_free (c);
}

void
conversation_add (Conversation *c, const char *role, const char *content,
                  const char *think, const char *stats)
{
  StoreMsg *m = g_new0 (StoreMsg, 1);
  m->role = g_strdup (role);
  m->content = g_strdup (content);
  m->think = think && *think ? g_strdup (think) : NULL;
  m->stats = stats && *stats ? g_strdup (stats) : NULL;
  g_ptr_array_add (c->msgs, m);
  c->updated = g_get_real_time () / G_USEC_PER_SEC;
}

void
conversation_begin_sources (Conversation *c)
{
  if (c->msgs->len == 0)
    return;
  StoreMsg *m = c->msgs->pdata[c->msgs->len - 1];
  if (m->sources)
    g_ptr_array_unref (m->sources);
  m->sources = g_ptr_array_new_with_free_func (store_source_free);
}

void
conversation_add_source (Conversation *c, const char *file, int page, const char *text)
{
  if (c->msgs->len == 0)
    return;
  StoreMsg *m = c->msgs->pdata[c->msgs->len - 1];
  if (!m->sources)
    conversation_begin_sources (c);
  StoreSource *s = g_new0 (StoreSource, 1);
  s->file = g_strdup (file);
  s->page = page;
  s->text = g_strdup (text);
  g_ptr_array_add (m->sources, s);
}

void
conversation_set_note (Conversation *c, const char *note)
{
  if (c->msgs->len == 0)
    return;
  StoreMsg *m = c->msgs->pdata[c->msgs->len - 1];
  g_free (m->note);
  m->note = g_strdup (note);
}

void
conversation_add_tool (Conversation *c, const char *name, const char *args, const char *result, gboolean ok)
{
  if (c->msgs->len == 0)
    return;
  StoreMsg *m = c->msgs->pdata[c->msgs->len - 1];
  if (!m->tools)
    m->tools = g_ptr_array_new_with_free_func (store_tool_free);
  StoreTool *t = g_new0 (StoreTool, 1);
  t->name = g_strdup (name);
  t->args = g_strdup (args ? args : "");
  t->result = g_strdup (result ? result : "");
  t->ok = ok;
  g_ptr_array_add (m->tools, t);
}

gboolean
conversation_has_lib (const Conversation *c, const char *id)
{
  for (guint i = 0; i < c->libs->len; i++)
    if (g_str_equal (c->libs->pdata[i], id))
      return TRUE;
  return FALSE;
}

void
conversation_toggle_lib (Conversation *c, const char *id)
{
  for (guint i = 0; i < c->libs->len; i++)
    if (g_str_equal (c->libs->pdata[i], id))
      {
        g_ptr_array_remove_index (c->libs, i);
        return;
      }
  g_ptr_array_add (c->libs, g_strdup (id));
}

void
conversation_set_author (Conversation *c, const char *name, const char *emoji)
{
  if (c->msgs->len == 0)
    return;
  StoreMsg *m = c->msgs->pdata[c->msgs->len - 1];
  g_free (m->author);
  g_free (m->author_emoji);
  m->author = g_strdup (name);
  m->author_emoji = g_strdup (emoji);
}

static char *
chats_dir (void)
{
  return g_build_filename (g_get_user_data_dir (), "npu-chat", "chats", NULL);
}

static char *
chat_path (const char *id)
{
  g_autofree char *dir = chats_dir ();
  g_autofree char *file = g_strconcat (id, ".json", NULL);
  return g_build_filename (dir, file, NULL);
}

static const char *
str_member (JsonObject *o, const char *name)
{
  return json_object_get_string_member_with_default (o, name, NULL);
}

static Conversation *
load_file (const char *path, gboolean with_messages)
{
  g_autoptr (JsonParser) parser = json_parser_new ();
  if (!json_parser_load_from_file (parser, path, NULL))
    return NULL;

  JsonNode *root = json_parser_get_root (parser);
  if (!JSON_NODE_HOLDS_OBJECT (root))
    return NULL;
  JsonObject *o = json_node_get_object (root);
  if (!str_member (o, "id"))
    return NULL;

  Conversation *c = g_new0 (Conversation, 1);
  c->id = g_strdup (str_member (o, "id"));
  c->title = g_strdup (str_member (o, "title"));
  c->model = g_strdup (str_member (o, "model"));
  c->assistant_id = g_strdup (str_member (o, "assistant_id"));
  c->assistant_name = g_strdup (str_member (o, "assistant"));
  c->assistant_emoji = g_strdup (str_member (o, "assistant_emoji"));
  c->system = g_strdup (str_member (o, "system"));
  c->updated = json_object_get_int_member_with_default (o, "updated", 0);
  c->msgs = g_ptr_array_new_with_free_func (store_msg_free);
  c->team = g_ptr_array_new_with_free_func (team_member_free);
  c->libs = g_ptr_array_new_with_free_func (g_free);
  JsonNode *libs = json_object_get_member (o, "libs");
  for (guint i = 0; libs && JSON_NODE_HOLDS_ARRAY (libs) && i < json_array_get_length (json_node_get_array (libs)); i++)
    {
      const char *id = json_array_get_string_element (json_node_get_array (libs), i);
      if (id)
        g_ptr_array_add (c->libs, g_strdup (id));
    }
  JsonNode *team = json_object_get_member (o, "team");
  for (guint i = 0; team && JSON_NODE_HOLDS_ARRAY (team) && i < json_array_get_length (json_node_get_array (team)); i++)
    {
      JsonObject *t = json_array_get_object_element (json_node_get_array (team), i);
      if (t && str_member (t, "name"))
        g_ptr_array_add (c->team, team_member_new (str_member (t, "id"), str_member (t, "name"),
                                                   str_member (t, "emoji"), str_member (t, "instructions")));
    }
  c->loaded = with_messages;
  if (!with_messages)
    return c;

  JsonArray *msgs = json_object_has_member (o, "messages")
                      ? json_object_get_array_member (o, "messages") : NULL;
  for (guint i = 0; msgs && i < json_array_get_length (msgs); i++)
    {
      JsonObject *m = json_array_get_object_element (msgs, i);
      if (!m || !str_member (m, "role") || !str_member (m, "content"))
        continue;
      StoreMsg *sm = g_new0 (StoreMsg, 1);
      sm->role = g_strdup (str_member (m, "role"));
      sm->content = g_strdup (str_member (m, "content"));
      sm->think = g_strdup (str_member (m, "think"));
      sm->stats = g_strdup (str_member (m, "stats"));
      JsonNode *src = json_object_get_member (m, "sources");
      if (src && JSON_NODE_HOLDS_ARRAY (src))
        {
          sm->sources = g_ptr_array_new_with_free_func (store_source_free);
          for (guint k = 0; k < json_array_get_length (json_node_get_array (src)); k++)
            {
              JsonObject *so = json_array_get_object_element (json_node_get_array (src), k);
              if (!so)
                continue;
              StoreSource *s = g_new0 (StoreSource, 1);
              s->file = g_strdup (str_member (so, "f"));
              s->page = (int) json_object_get_int_member_with_default (so, "p", 0);
              s->text = g_strdup (str_member (so, "t"));
              g_ptr_array_add (sm->sources, s);
            }
        }
      JsonNode *tl = json_object_get_member (m, "tools");
      if (tl && JSON_NODE_HOLDS_ARRAY (tl))
        {
          sm->tools = g_ptr_array_new_with_free_func (store_tool_free);
          for (guint k = 0; k < json_array_get_length (json_node_get_array (tl)); k++)
            {
              JsonObject *to = json_array_get_object_element (json_node_get_array (tl), k);
              if (!to)
                continue;
              StoreTool *t = g_new0 (StoreTool, 1);
              t->name = g_strdup (str_member (to, "n"));
              t->args = g_strdup (str_member (to, "a"));
              t->result = g_strdup (str_member (to, "r"));
              t->ok = json_object_get_boolean_member_with_default (to, "ok", TRUE);
              g_ptr_array_add (sm->tools, t);
            }
        }
      sm->note = g_strdup (str_member (m, "note"));
      sm->author = g_strdup (str_member (m, "author"));
      sm->author_emoji = g_strdup (str_member (m, "author_emoji"));
      g_ptr_array_add (c->msgs, sm);
    }
  return c;
}

static int
by_updated_desc (gconstpointer a, gconstpointer b)
{
  const Conversation *ca = *(Conversation **) a;
  const Conversation *cb = *(Conversation **) b;
  return (cb->updated > ca->updated) - (cb->updated < ca->updated);
}

GPtrArray *
store_load_all (void)
{
  GPtrArray *all = g_ptr_array_new_with_free_func ((GDestroyNotify) conversation_free);
  g_autofree char *dir = chats_dir ();
  g_autoptr (GDir) d = g_dir_open (dir, 0, NULL);
  const char *name;

  while (d && (name = g_dir_read_name (d)))
    {
      if (!g_str_has_suffix (name, ".json"))
        continue;
      g_autofree char *path = g_build_filename (dir, name, NULL);
      Conversation *c = load_file (path, FALSE);
      if (c)
        g_ptr_array_add (all, c);
    }

  g_ptr_array_sort (all, by_updated_desc);
  return all;
}

void
store_load_messages (Conversation *c)
{
  if (c->loaded)
    return;
  g_autofree char *path = chat_path (c->id);
  Conversation *full = load_file (path, TRUE);
  c->loaded = TRUE;
  if (!full)
    return;
  /* Swap the message arrays and drop the temporary copy. */
  GPtrArray *tmp = c->msgs;
  c->msgs = full->msgs;
  full->msgs = tmp;
  conversation_free (full);
}

void
store_unload_messages (Conversation *c)
{
  g_ptr_array_set_size (c->msgs, 0);
  c->loaded = FALSE;
}

void
store_save (Conversation *c)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "id");
  json_builder_add_string_value (b, c->id);
  json_builder_set_member_name (b, "title");
  json_builder_add_string_value (b, c->title ? c->title : "");
  json_builder_set_member_name (b, "model");
  json_builder_add_string_value (b, c->model ? c->model : "");
  if (c->assistant_name)
    {
      json_builder_set_member_name (b, "assistant_id");
      json_builder_add_string_value (b, c->assistant_id ? c->assistant_id : "");
      json_builder_set_member_name (b, "assistant");
      json_builder_add_string_value (b, c->assistant_name);
      json_builder_set_member_name (b, "assistant_emoji");
      json_builder_add_string_value (b, c->assistant_emoji ? c->assistant_emoji : "");
    }
  if (c->system)
    {
      json_builder_set_member_name (b, "system");
      json_builder_add_string_value (b, c->system);
    }
  if (c->libs->len)
    {
      json_builder_set_member_name (b, "libs");
      json_builder_begin_array (b);
      for (guint i = 0; i < c->libs->len; i++)
        json_builder_add_string_value (b, c->libs->pdata[i]);
      json_builder_end_array (b);
    }
  if (c->team->len)
    {
      json_builder_set_member_name (b, "team");
      json_builder_begin_array (b);
      for (guint i = 0; i < c->team->len; i++)
        {
          TeamMember *t = c->team->pdata[i];
          json_builder_begin_object (b);
          json_builder_set_member_name (b, "id");
          json_builder_add_string_value (b, t->id ? t->id : "");
          json_builder_set_member_name (b, "name");
          json_builder_add_string_value (b, t->name);
          json_builder_set_member_name (b, "emoji");
          json_builder_add_string_value (b, t->emoji ? t->emoji : "");
          json_builder_set_member_name (b, "instructions");
          json_builder_add_string_value (b, t->instructions);
          json_builder_end_object (b);
        }
      json_builder_end_array (b);
    }
  json_builder_set_member_name (b, "updated");
  json_builder_add_int_value (b, c->updated);
  json_builder_set_member_name (b, "messages");
  json_builder_begin_array (b);
  for (guint i = 0; i < c->msgs->len; i++)
    {
      StoreMsg *m = c->msgs->pdata[i];
      json_builder_begin_object (b);
      json_builder_set_member_name (b, "role");
      json_builder_add_string_value (b, m->role);
      json_builder_set_member_name (b, "content");
      json_builder_add_string_value (b, m->content);
      if (m->think)
        {
          json_builder_set_member_name (b, "think");
          json_builder_add_string_value (b, m->think);
        }
      if (m->stats)
        {
          json_builder_set_member_name (b, "stats");
          json_builder_add_string_value (b, m->stats);
        }
      if (m->sources)
        {
          json_builder_set_member_name (b, "sources");
          json_builder_begin_array (b);
          for (guint k = 0; k < m->sources->len; k++)
            {
              StoreSource *s = m->sources->pdata[k];
              json_builder_begin_object (b);
              json_builder_set_member_name (b, "f");
              json_builder_add_string_value (b, s->file ? s->file : "");
              json_builder_set_member_name (b, "p");
              json_builder_add_int_value (b, s->page);
              json_builder_set_member_name (b, "t");
              json_builder_add_string_value (b, s->text ? s->text : "");
              json_builder_end_object (b);
            }
          json_builder_end_array (b);
        }
      if (m->note)
        {
          json_builder_set_member_name (b, "note");
          json_builder_add_string_value (b, m->note);
        }
      if (m->tools)
        {
          json_builder_set_member_name (b, "tools");
          json_builder_begin_array (b);
          for (guint k = 0; k < m->tools->len; k++)
            {
              StoreTool *t = m->tools->pdata[k];
              json_builder_begin_object (b);
              json_builder_set_member_name (b, "n");
              json_builder_add_string_value (b, t->name ? t->name : "");
              json_builder_set_member_name (b, "a");
              json_builder_add_string_value (b, t->args ? t->args : "");
              json_builder_set_member_name (b, "r");
              json_builder_add_string_value (b, t->result ? t->result : "");
              json_builder_set_member_name (b, "ok");
              json_builder_add_boolean_value (b, t->ok);
              json_builder_end_object (b);
            }
          json_builder_end_array (b);
        }
      if (m->author)
        {
          json_builder_set_member_name (b, "author");
          json_builder_add_string_value (b, m->author);
          json_builder_set_member_name (b, "author_emoji");
          json_builder_add_string_value (b, m->author_emoji ? m->author_emoji : "");
        }
      json_builder_end_object (b);
    }
  json_builder_end_array (b);
  json_builder_end_object (b);

  g_autoptr (JsonNode) root = json_builder_get_root (b);
  g_autofree char *json = json_to_string (root, TRUE);
  g_autofree char *dir = chats_dir ();
  g_autofree char *path = chat_path (c->id);
  g_mkdir_with_parents (dir, 0700);
  g_file_set_contents_full (path, json, -1, G_FILE_SET_CONTENTS_CONSISTENT, 0600, NULL);
}

void
store_delete (Conversation *c)
{
  g_autofree char *path = chat_path (c->id);
  g_unlink (path);
}
