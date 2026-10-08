#include "assistants.h"

#include <json-glib/json-glib.h>

#include "i18n.h"
#include "templates.h"

Assistant *
assistant_new (const char *name, const char *emoji, const char *instructions)
{
  Assistant *a = g_new0 (Assistant, 1);
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  g_autofree char *stamp = g_date_time_format (now, "%Y%m%d%H%M%S");

  a->id = g_strdup_printf ("%s-%04x", stamp, g_random_int_range (0, 0xffff));
  a->name = g_strdup (name);
  a->emoji = g_strdup (emoji);
  a->instructions = g_strdup (instructions);
  a->libs = g_ptr_array_new_with_free_func (g_free);
  return a;
}

void
assistant_free (Assistant *a)
{
  if (!a)
    return;
  g_free (a->id);
  g_free (a->name);
  g_free (a->emoji);
  g_free (a->instructions);
  g_free (a->template_key);
  if (a->libs)
    g_ptr_array_unref (a->libs);
  g_free (a);
}

static char *
assistants_path (void)
{
  return g_build_filename (g_get_user_data_dir (), "npu-chat", "assistants.json", NULL);
}

Assistant *
assistant_new_from_template (const char *key)
{
  const AssistantTemplate *t = template_find (key);
  if (!t)
    return NULL;
  Assistant *a = assistant_new (TR (t->name_es, t->name_en), t->emoji, TR (t->instr_es, t->instr_en));
  a->template_key = g_strdup (key);
  return a;
}

gboolean
assistants_has_template (GPtrArray *list, const char *key)
{
  for (guint i = 0; i < list->len; i++)
    if (g_strcmp0 (((Assistant *) list->pdata[i])->template_key, key) == 0)
      return TRUE;
  return FALSE;
}

static GPtrArray *
seed_examples (void)
{
  static const char *keys[] = { "psychologist", "strategist", "teacher" };
  GPtrArray *list = g_ptr_array_new_with_free_func ((GDestroyNotify) assistant_free);
  for (guint i = 0; i < G_N_ELEMENTS (keys); i++)
    g_ptr_array_add (list, assistant_new_from_template (keys[i]));
  return list;
}

static const char *
str_member (JsonObject *o, const char *name)
{
  return json_object_get_string_member_with_default (o, name, NULL);
}

GPtrArray *
assistants_load (void)
{
  g_autofree char *path = assistants_path ();
  g_autoptr (JsonParser) parser = json_parser_new ();

  if (!g_file_test (path, G_FILE_TEST_EXISTS))
    {
      GPtrArray *seeded = seed_examples ();
      assistants_save (seeded);
      return seeded;
    }

  GPtrArray *list = g_ptr_array_new_with_free_func ((GDestroyNotify) assistant_free);
  if (!json_parser_load_from_file (parser, path, NULL))
    return list;

  JsonNode *root = json_parser_get_root (parser);
  if (!JSON_NODE_HOLDS_ARRAY (root))
    return list;

  JsonArray *arr = json_node_get_array (root);
  for (guint i = 0; i < json_array_get_length (arr); i++)
    {
      JsonObject *o = json_array_get_object_element (arr, i);
      if (!o || !str_member (o, "id") || !str_member (o, "name"))
        continue;
      Assistant *a = g_new0 (Assistant, 1);
      a->id = g_strdup (str_member (o, "id"));
      a->name = g_strdup (str_member (o, "name"));
      a->emoji = g_strdup (str_member (o, "emoji") ? str_member (o, "emoji") : "✦");
      a->instructions = g_strdup (str_member (o, "instructions") ? str_member (o, "instructions") : "");
      a->template_key = g_strdup (str_member (o, "template"));
      a->libs = g_ptr_array_new_with_free_func (g_free);
      JsonNode *ln = json_object_get_member (o, "libs");
      for (guint k = 0; ln && JSON_NODE_HOLDS_ARRAY (ln) && k < json_array_get_length (json_node_get_array (ln)); k++)
        {
          const char *lid = json_array_get_string_element (json_node_get_array (ln), k);
          if (lid)
            g_ptr_array_add (a->libs, g_strdup (lid));
        }
      /* Lists saved before the gallery existed: recognise unchanged examples. */
      for (guint t = 0; !a->template_key && t < assistant_templates_count; t++)
        {
          const AssistantTemplate *tp = &assistant_templates[t];
          if (g_str_equal (a->emoji, tp->emoji) &&
              (g_str_equal (a->name, tp->name_es) || g_str_equal (a->name, tp->name_en)))
            a->template_key = g_strdup (tp->key);
        }
      g_ptr_array_add (list, a);
    }
  return list;
}

void
assistants_save (GPtrArray *list)
{
  g_autoptr (JsonBuilder) b = json_builder_new ();
  json_builder_begin_array (b);
  for (guint i = 0; i < list->len; i++)
    {
      Assistant *a = list->pdata[i];
      json_builder_begin_object (b);
      json_builder_set_member_name (b, "id");
      json_builder_add_string_value (b, a->id);
      json_builder_set_member_name (b, "name");
      json_builder_add_string_value (b, a->name);
      json_builder_set_member_name (b, "emoji");
      json_builder_add_string_value (b, a->emoji);
      json_builder_set_member_name (b, "instructions");
      json_builder_add_string_value (b, a->instructions);
      if (a->libs && a->libs->len)
        {
          json_builder_set_member_name (b, "libs");
          json_builder_begin_array (b);
          for (guint k = 0; k < a->libs->len; k++)
            json_builder_add_string_value (b, a->libs->pdata[k]);
          json_builder_end_array (b);
        }
      if (a->template_key)
        {
          json_builder_set_member_name (b, "template");
          json_builder_add_string_value (b, a->template_key);
        }
      json_builder_end_object (b);
    }
  json_builder_end_array (b);

  g_autoptr (JsonNode) root = json_builder_get_root (b);
  g_autofree char *json = json_to_string (root, TRUE);
  g_autofree char *path = assistants_path ();
  g_autofree char *dir = g_path_get_dirname (path);
  g_mkdir_with_parents (dir, 0700);
  g_file_set_contents_full (path, json, -1, G_FILE_SET_CONTENTS_CONSISTENT, 0600, NULL);
}

Assistant *
assistants_find (GPtrArray *list, const char *id)
{
  for (guint i = 0; id && i < list->len; i++)
    {
      Assistant *a = list->pdata[i];
      if (g_str_equal (a->id, id))
        return a;
    }
  return NULL;
}
