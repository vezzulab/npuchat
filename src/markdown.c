#include "markdown.h"

#include <pango/pango.h>
#include <string.h>

#define MAX_OPEN 8

static void
md_block_free (gpointer p)
{
  MdBlock *b = p;
  g_free (b->text);
  g_free (b->lang);
  g_free (b);
}

static void
append_escaped (GString *out, const char *s, gssize len)
{
  g_autofree char *esc = g_markup_escape_text (s, len);
  g_string_append (out, esc);
}

static const char *
tag_name (char t)
{
  switch (t)
    {
    case 'b': return "b";
    case 'i': return "i";
    default:  return "s";
    }
}

typedef struct {
  char open[MAX_OPEN];
  int n;
} TagStack;

/* Opens or closes a tag. Closing a tag that is not on top would produce
 * invalid markup, so in that case the marker is kept as literal text. */
static void
toggle (GString *out, TagStack *st, char tag, const char *marker)
{
  if (st->n > 0 && st->open[st->n - 1] == tag)
    {
      st->n--;
      g_string_append_printf (out, "</%s>", tag_name (tag));
      return;
    }
  for (int i = 0; i < st->n; i++)
    if (st->open[i] == tag)
      {
        append_escaped (out, marker, -1);
        return;
      }
  if (st->n == MAX_OPEN)
    {
      append_escaped (out, marker, -1);
      return;
    }
  st->open[st->n++] = tag;
  g_string_append_printf (out, "<%s>", tag_name (tag));
}

static gboolean
is_word_char (char c)
{
  return g_ascii_isalnum (c) || (unsigned char) c >= 0x80;
}

static void
render_inline (GString *out, const char *s)
{
  TagStack st = { 0 };
  const char *p = s;

  while (*p)
    {
      char prev = p > s ? p[-1] : ' ';

      if (*p == '`')
        {
          const char *q = strchr (p + 1, '`');
          if (q)
            {
              g_string_append (out, "<tt>");
              append_escaped (out, p + 1, q - p - 1);
              g_string_append (out, "</tt>");
              p = q + 1;
              continue;
            }
        }
      else if ((p[0] == '*' && p[1] == '*') || (p[0] == '_' && p[1] == '_' && !is_word_char (prev)))
        {
          char marker[3] = { p[0], p[1], 0 };
          toggle (out, &st, 'b', marker);
          p += 2;
          continue;
        }
      else if (p[0] == '~' && p[1] == '~')
        {
          toggle (out, &st, 's', "~~");
          p += 2;
          continue;
        }
      else if (*p == '*' || *p == '_')
        {
          gboolean closing = st.n > 0 && st.open[st.n - 1] == 'i' && prev != ' ';
          gboolean opening = p[1] && p[1] != ' ' && !is_word_char (prev);
          if (*p == '_' && closing && is_word_char (p[1]))
            closing = FALSE;
          if (closing || opening)
            {
              char marker[2] = { *p, 0 };
              toggle (out, &st, 'i', marker);
              p++;
              continue;
            }
        }
      else if (*p == '[')
        {
          const char *close = strstr (p, "](");
          const char *end = close ? strchr (close + 2, ')') : NULL;
          if (close && end && !memchr (p + 1, '[', close - p - 1))
            {
              g_autofree char *url = g_strndup (close + 2, end - close - 2);
              g_autofree char *url_esc = g_markup_escape_text (url, -1);
              g_string_append_printf (out, "<a href=\"%s\">", url_esc);
              append_escaped (out, p + 1, close - p - 1);
              g_string_append (out, "</a>");
              p = end + 1;
              continue;
            }
        }

      const char *next = g_utf8_next_char (p);
      append_escaped (out, p, next - p);
      p = next;
    }

  while (st.n > 0)
    g_string_append_printf (out, "</%s>", tag_name (st.open[--st.n]));
}

static gboolean
is_table_separator (const char *s)
{
  for (; *s; s++)
    if (!strchr ("|-: \t", *s))
      return FALSE;
  return TRUE;
}

static gboolean
is_rule (const char *s)
{
  char c = *s;
  int n = 0;
  if (c != '-' && c != '*' && c != '_')
    return FALSE;
  for (; *s; s++)
    {
      if (*s == c)
        n++;
      else if (*s != ' ')
        return FALSE;
    }
  return n >= 3;
}

static void
render_line (GString *out, const char *line)
{
  const char *t = line;
  int indent = 0;

  while (*t == ' ' || *t == '\t')
    {
      indent += *t == '\t' ? 4 : 1;
      t++;
    }

  if (*t == '|' && is_table_separator (t))
    return;

  if (out->len > 0)
    g_string_append_c (out, '\n');

  int hashes = 0;
  while (t[hashes] == '#')
    hashes++;

  if (hashes >= 1 && hashes <= 6 && t[hashes] == ' ')
    {
      const char *size = hashes == 1 ? "x-large" : hashes == 2 ? "large" : "medium";
      g_string_append_printf (out, "<span size=\"%s\" weight=\"bold\">", size);
      render_inline (out, t + hashes + 1);
      g_string_append (out, "</span>");
    }
  else if (is_rule (t))
    {
      g_string_append (out, "<span fgalpha=\"35%\">────────────────────</span>");
    }
  else if ((*t == '-' || *t == '*' || *t == '+') && t[1] == ' ')
    {
      for (int i = 0; i < indent / 2; i++)
        g_string_append (out, "    ");
      g_string_append (out, "  •  ");
      render_inline (out, t + 2);
    }
  else if (g_ascii_isdigit (*t))
    {
      const char *d = t;
      while (g_ascii_isdigit (*d))
        d++;
      if ((*d == '.' || *d == ')') && d[1] == ' ')
        {
          for (int i = 0; i < indent / 2; i++)
            g_string_append (out, "    ");
          g_string_append (out, "  <b>");
          append_escaped (out, t, d - t + 1);
          g_string_append (out, "</b> ");
          render_inline (out, d + 2);
        }
      else
        render_inline (out, line);
    }
  else if (*t == '>')
    {
      g_string_append (out, "<span fgalpha=\"70%\">▍ <i>");
      render_inline (out, t[1] == ' ' ? t + 2 : t + 1);
      g_string_append (out, "</i></span>");
    }
  else if (*t == '|')
    {
      g_string_append (out, "<tt>");
      append_escaped (out, t, -1);
      g_string_append (out, "</tt>");
    }
  else
    {
      render_inline (out, line);
    }
}

/* GtkLabel accepts <a href>, but pango_parse_markup() does not, so links
 * are swapped for spans before validating. */
static gboolean
markup_is_valid (const char *markup)
{
  static GRegex *open_a, *close_a;
  if (!open_a)
    {
      open_a = g_regex_new ("<a href=\"[^\"]*\">", 0, 0, NULL);
      close_a = g_regex_new ("</a>", 0, 0, NULL);
    }
  g_autofree char *tmp = g_regex_replace_literal (open_a, markup, -1, 0, "<span>", 0, NULL);
  g_autofree char *check = g_regex_replace_literal (close_a, tmp, -1, 0, "</span>", 0, NULL);
  return pango_parse_markup (check, -1, 0, NULL, NULL, NULL, NULL);
}

#define PARA_GAP "\n<span size=\"5pt\"> </span>"

static void
flush_text (GPtrArray *blocks, GString *markup, GString *raw)
{
  while (g_str_has_suffix (markup->str, PARA_GAP))
    g_string_truncate (markup, markup->len - strlen (PARA_GAP));

  if (markup->len > 0)
    {
      MdBlock *b = g_new0 (MdBlock, 1);
      b->kind = MD_TEXT;
      /* Never hand broken markup to GtkLabel: fall back to plain text. */
      if (markup_is_valid (markup->str))
        b->text = g_strdup (markup->str);
      else
        b->text = g_markup_escape_text (g_strstrip (raw->str), -1);
      g_ptr_array_add (blocks, b);
    }

  g_string_truncate (markup, 0);
  g_string_truncate (raw, 0);
}

GPtrArray *
md_parse (const char *src)
{
  GPtrArray *blocks = g_ptr_array_new_with_free_func (md_block_free);
  g_auto (GStrv) lines = g_strsplit (src, "\n", -1);
  g_autoptr (GString) markup = g_string_new (NULL);
  g_autoptr (GString) raw = g_string_new (NULL);
  GString *code = NULL;
  char *lang = NULL;
  gboolean prev_blank = TRUE;

  for (int i = 0; lines[i]; i++)
    {
      char *line = lines[i];
      gsize len = strlen (line);
      if (len > 0 && line[len - 1] == '\r')
        line[len - 1] = '\0';

      const char *t = line;
      while (*t == ' ')
        t++;

      if (g_str_has_prefix (t, "```"))
        {
          if (code)
            {
              MdBlock *b = g_new0 (MdBlock, 1);
              b->kind = MD_CODE;
              b->text = g_string_free (code, FALSE);
              b->lang = lang;
              g_ptr_array_add (blocks, b);
              code = NULL;
              lang = NULL;
            }
          else
            {
              flush_text (blocks, markup, raw);
              code = g_string_new (NULL);
              lang = g_strstrip (g_strdup (t + 3));
            }
          prev_blank = TRUE;
          continue;
        }

      if (code)
        {
          if (code->len > 0)
            g_string_append_c (code, '\n');
          g_string_append (code, line);
          continue;
        }

      gboolean blank = *t == '\0';
      if (blank && (prev_blank || markup->len == 0))
        continue;
      prev_blank = blank;

      g_string_append (raw, line);
      g_string_append_c (raw, '\n');
      if (blank)
        g_string_append (markup, PARA_GAP);
      else
        render_line (markup, line);
    }

  if (code)
    {
      /* Unterminated fence while the reply is still streaming. */
      MdBlock *b = g_new0 (MdBlock, 1);
      b->kind = MD_CODE;
      b->text = g_string_free (code, FALSE);
      b->lang = lang;
      g_ptr_array_add (blocks, b);
    }
  else
    {
      flush_text (blocks, markup, raw);
    }

  return blocks;
}
