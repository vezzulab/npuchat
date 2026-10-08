/* Tests the retrieval engine on its own: chunking, tokenizing, importing
 * text/Markdown/PDF files, persistence, search and removal.
 * Built with -Dtests=true and run by tests/run-selftest.sh. */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "../src/rag.h"

static int failures;

#define CHECK(cond, ...)                                                       \
  do {                                                                         \
    if (!(cond))                                                               \
      {                                                                        \
        failures++;                                                            \
        g_printerr ("FAIL %s:%d: ", __FILE__, __LINE__);                       \
        g_printerr (__VA_ARGS__);                                              \
        g_printerr ("\n");                                                     \
      }                                                                        \
  } while (0)

/* Support-manual paragraphs about unrelated topics, as in the real-model
 * experiment that picked this design. */
static const char *manual[] = {
  "La garantía del producto cubre defectos de fabricación durante 24 meses desde la compra. No cubre daños por agua ni golpes.",
  "Para solicitar un reembolso, envía el recibo y el producto sin usar dentro de los 30 días posteriores a la compra.",
  "El router se reinicia manteniendo presionado el botón trasero durante 10 segundos hasta que las luces parpadeen.",
  "La batería de 5000 mAh dura unas 14 horas de uso normal y se carga al 80 % en 40 minutos.",
  "Los envíos internacionales tardan entre 7 y 15 días hábiles y cuestan más según el país de destino.",
  "El panel solar produce en promedio 4 kWh diarios con buena exposición y requiere limpieza cada tres meses.",
  "Para cambiar la contraseña entra a Ajustes, Seguridad y pulsa Cambiar contraseña; debe tener al menos 12 caracteres.",
  "El servicio técnico atiende de lunes a viernes de 8 a 18 horas y los sábados hasta el mediodía.",
  "La aspiradora robot recorre la casa en 90 minutos y regresa sola a la base cuando la carga baja del 15 %.",
  "El horno se limpia con la función de pirólisis, que alcanza 480 grados y tarda aproximadamente dos horas.",
  "Para emparejar los auriculares, mantén pulsado el botón durante 5 segundos hasta ver la luz azul intermitente.",
  "La cámara graba en 4K a 60 cuadros por segundo y guarda los videos en la tarjeta de memoria SD.",
  "El plan anual incluye 1 terabyte de almacenamiento en la nube y soporte prioritario por chat.",
  "Para cancelar la suscripción, ve a Facturación y elige Cancelar plan; el acceso continúa hasta el fin del ciclo.",
  "La lavadora tiene un programa rápido de 15 minutos para prendas poco sucias y un programa de lana a 30 grados.",
  "El termostato inteligente aprende tus horarios en una semana y reduce el consumo de calefacción hasta un 20 %.",
  "Si la pantalla no enciende, conecta el cargador por 30 minutos y mantén pulsado el botón de encendido 15 segundos.",
  "La bicicleta eléctrica alcanza 25 kilómetros por hora con asistencia y tiene una autonomía de 70 kilómetros.",
  "Para actualizar el firmware, descarga el archivo desde la web de soporte y cópialo a una memoria USB vacía.",
  "El filtro de agua debe sustituirse cada seis meses o después de filtrar 500 litros, lo que ocurra primero.",
  "El altavoz es resistente al agua con certificación IP67 y flota durante unos minutos si cae en una piscina.",
  "La impresora admite papel A4 y carta y puede imprimir a doble cara de forma automática.",
  "Los puntos de fidelidad caducan a los 12 meses y se canjean por descuentos en la tienda en línea.",
  "El sensor de movimiento detecta personas hasta 8 metros y envía una alerta al teléfono en menos de un segundo.",
};

/* Question + the keywords qwen3.5:9b produced for it on the real NPU
 * (see the project notes), and the paragraph that must come first. */
static const struct {
  const char *query;
  guint       want;
} cases[] = {
  { "¿Cuánto tiempo cubre la garantía? garantía tiempo cobertura duración periodo vigencia amparo", 0 },
  { "¿Cómo me devuelven el dinero? devolución dinero reembolso política cambio garantía", 1 },
  { "¿Cómo reinicio el router? reinicio router resetear restablecer botón reset factory default", 2 },
  { "¿Cuánto dura la batería? batería duración vida autonomía horas carga recarga", 3 },
  { "¿En cuántos días llega un pedido internacional? tiempo de entrega internacional días envío global plazos", 4 },
  { "¿Cómo cambio mi clave de acceso? cambiar clave de acceso contraseña login usuario credenciales", 6 },
  { "¿Cuándo atienden en soporte? soporte horario atención disponibilidad servicio técnico", 7 },
  { "¿Cómo conecto los audífonos por bluetooth? conectar audífonos bluetooth emparejar inalámbricos", 10 },
  { "¿Cómo doy de baja mi plan? cancelar plan cancelar suscripción finalizar servicio dar de baja", 13 },
  { "La pantalla no prende, ¿qué hago? pantalla no enciende, pantalla apagada, sin imagen", 16 },
  { "¿Cada cuánto cambio el filtro de agua? filtro de agua frecuencia cambio intervalo reemplazo vida útil", 19 },
  { "¿Cuándo vencen mis puntos? vencimiento puntos expiración caducidad validez fecha límite", 22 },
};

/* A valid two-page PDF with a text layer, built by hand. */
static char *
make_pdf (const char *page1, const char *page2, gsize *out_len)
{
  GString *pdf = g_string_new ("%PDF-1.4\n");
  gsize offsets[8] = { 0 };
  g_autofree char *c1 = g_strdup_printf ("BT /F1 12 Tf 72 720 Td (%s) Tj ET", page1);
  g_autofree char *c2 = g_strdup_printf ("BT /F1 12 Tf 72 720 Td (%s) Tj ET", page2);

#define OBJ(n, ...) do { offsets[n] = pdf->len; g_string_append_printf (pdf, "%d 0 obj\n", n); \
                         g_string_append_printf (pdf, __VA_ARGS__); g_string_append (pdf, "\nendobj\n"); } while (0)
  OBJ (1, "<< /Type /Catalog /Pages 2 0 R >>");
  OBJ (2, "<< /Type /Pages /Kids [3 0 R 5 0 R] /Count 2 >>");
  OBJ (3, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R /Resources << /Font << /F1 7 0 R >> >> >>");
  OBJ (4, "<< /Length %zu >>\nstream\n%s\nendstream", strlen (c1), c1);
  OBJ (5, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 6 0 R /Resources << /Font << /F1 7 0 R >> >> >>");
  OBJ (6, "<< /Length %zu >>\nstream\n%s\nendstream", strlen (c2), c2);
  OBJ (7, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");
#undef OBJ

  gsize xref = pdf->len;
  g_string_append (pdf, "xref\n0 8\n0000000000 65535 f \n");
  for (int i = 1; i < 8; i++)
    g_string_append_printf (pdf, "%010zu 00000 n \n", offsets[i]);
  g_string_append_printf (pdf, "trailer\n<< /Size 8 /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF\n", xref);
  *out_len = pdf->len;
  return g_string_free (pdf, FALSE);
}

typedef struct {
  GMainLoop *loop;
  guint      added;
  GStrv      errors;
  guint      progress_calls;
} ImportResult;

static void
on_progress (double fraction, const char *file, gpointer data)
{
  (void) fraction;
  (void) file;
  ((ImportResult *) data)->progress_calls++;
}

static void
on_done (guint added, char **errors, gpointer data)
{
  ImportResult *r = data;
  r->added = added;
  r->errors = g_strdupv (errors);
  g_main_loop_quit (r->loop);
}

static ImportResult
import (RagLibrary *lib, char **paths)
{
  ImportResult r = { .loop = g_main_loop_new (NULL, FALSE) };
  rag_library_add_files (lib, paths, on_progress, on_done, &r);
  g_main_loop_run (r.loop);
  g_main_loop_unref (r.loop);
  return r;
}

static const char *
top_file (RagIndex *ix, const char *query, int *page)
{
  static char buf[256];
  g_autoptr (GPtrArray) hits = rag_search (ix, query, 4);
  if (hits->len == 0)
    return NULL;
  RagHit *h = hits->pdata[0];
  g_strlcpy (buf, h->file, sizeof buf);
  if (page)
    *page = h->page;
  return buf;
}

static void
test_chunker (void)
{
  /* Headings are carried into chunks, long text is split with overlap. */
  g_autoptr (GString) long_text = g_string_new ("# Instalación\n\n");
  for (int i = 0; i < 60; i++)
    g_string_append_printf (long_text, "Esta es la oración número %d del manual de instalación del equipo. ", i);
  g_autoptr (GPtrArray) chunks = rag_chunk_text (long_text->str, 0);
  CHECK (chunks->len >= 3, "long text should split into several chunks, got %u", chunks->len);
  for (guint i = 0; i < chunks->len; i++)
    {
      const char *c = chunks->pdata[i];
      CHECK (strlen (c) < 1900, "chunk %u too long (%zu)", i, strlen (c));
      CHECK (g_str_has_prefix (c, "Instalación"), "chunk %u lost its heading", i);
      CHECK (g_utf8_validate (c, -1, NULL), "chunk %u is not valid UTF-8", i);
    }

  g_autoptr (GPtrArray) tiny = rag_chunk_text ("ok", 0);
  CHECK (tiny->len == 0, "fragments shorter than the minimum are dropped");

  g_autoptr (GString) nospace = g_string_new (NULL);
  for (int i = 0; i < 4000; i++)
    g_string_append (nospace, "ñ"); /* no spaces, multibyte */
  g_autoptr (GPtrArray) hard = rag_chunk_text (nospace->str, 0);
  CHECK (hard->len >= 2, "a long unbroken string is still split");
  for (guint i = 0; i < hard->len; i++)
    CHECK (g_utf8_validate (hard->pdata[i], -1, NULL), "hard split broke a UTF-8 sequence");
}

static void
test_tokenizer (void)
{
  g_auto (GStrv) a = rag_tokenize ("¿Cómo CANCELAR la suscripción?");
  g_auto (GStrv) b = rag_tokenize ("cancelación de la suscripcion");
  CHECK (g_strv_length (a) == 2, "stop words are removed (got %u)", g_strv_length (a));
  CHECK (g_strcmp0 (a[0], b[0]) == 0, "'cancelar' and 'cancelación' meet: %s vs %s", a[0], b[0]);
  CHECK (g_strcmp0 (a[1], b[1]) == 0, "accents are ignored: %s vs %s", a[1], b[1]);
  g_auto (GStrv) none = rag_tokenize ("¿Cómo es el de la?");
  CHECK (g_strv_length (none) == 0, "a question made only of stop words has no terms");
}

int
main (void)
{
  g_autofree char *root = g_dir_make_tmp ("npu-chat-rag-XXXXXX", NULL);
  g_setenv ("XDG_DATA_HOME", root, TRUE);

  test_tokenizer ();
  test_chunker ();

  /* ---- fixtures: Markdown with headings, a text file, a PDF, a binary ---- */
  g_autofree char *md_path = g_build_filename (root, "manual.md", NULL);
  g_autofree char *txt_path = g_build_filename (root, "politica.txt", NULL);
  g_autofree char *bin_path = g_build_filename (root, "imagen.png", NULL);
  g_autofree char *pdf_path = g_build_filename (root, "catalogo.pdf", NULL);
  g_autofree char *fake_path = g_build_filename (root, "falso.txt", NULL);

  g_autoptr (GString) md = g_string_new ("# Manual de soporte\n\n");
  for (guint i = 0; i < G_N_ELEMENTS (manual); i++)
    g_string_append_printf (md, "%s\n\n", manual[i]);
  g_file_set_contents (md_path, md->str, -1, NULL);
  g_file_set_contents (txt_path, "Política de privacidad\n\nNo compartimos tus datos con terceros ni los vendemos.\n", -1, NULL);
  g_file_set_contents (bin_path, "\x89PNG\r\n\x1a\n", 8, NULL);
  g_file_set_contents (fake_path, "bytes\0binarios", 14, NULL);

  gsize pdf_len = 0;
  g_autofree char *pdf = make_pdf ("Catalogo de productos. Pagina uno trata de sillas y escritorios de oficina.",
                                   "Pagina dos trata del panel solar y los inversores fotovoltaicos de alta eficiencia.", &pdf_len);
  g_file_set_contents (pdf_path, pdf, pdf_len, NULL);

  /* ---- importing ---- */
  RagLibrary *lib = rag_library_new ("Pruebas", FALSE);
  char *paths[] = { md_path, txt_path, bin_path, fake_path, pdf_path, NULL };
  ImportResult r = import (lib, paths);
  guint expected = rag_pdf_available () ? 3 : 2;
  CHECK (r.added == expected, "expected %u documents, imported %u", expected, r.added);
  CHECK (g_strv_length (r.errors) >= 2, "the binary files should be reported (%u errors)", g_strv_length (r.errors));
  CHECK (r.progress_calls > 0, "progress was reported");
  CHECK (!lib->busy, "library is idle after import");
  CHECK (rag_library_chunk_count (lib) >= 6, "chunks were stored (%u)", rag_library_chunk_count (lib));
  g_strfreev (r.errors);

  /* ---- persistence: reload from disk ---- */
  g_autoptr (GPtrArray) libs = rag_libraries_load ();
  CHECK (libs->len == 1, "one library on disk, got %u", libs->len);
  RagLibrary *loaded = libs->pdata[0];
  CHECK (g_str_equal (loaded->name, "Pruebas"), "name persisted");
  CHECK (loaded->docs->len == expected, "documents persisted (%u)", loaded->docs->len);

  g_autoptr (GPtrArray) selection = g_ptr_array_new ();
  g_ptr_array_add (selection, loaded);
  RagIndex *ix = rag_index_new (selection);

  /* ---- retrieval quality on the 24-paragraph manual ---- */
  guint top1 = 0, top4 = 0;
  for (guint i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      g_autoptr (GPtrArray) hits = rag_search (ix, cases[i].query, 4);
      int found = -1;
      for (guint h = 0; h < hits->len && found < 0; h++)
        if (strstr (((RagHit *) hits->pdata[h])->text, manual[cases[i].want]))
          found = (int) h;
      top1 += found == 0;
      top4 += found >= 0;
      if (found < 0)
        g_printerr ("  miss: %s\n", cases[i].query);
    }
  g_print ("retrieval: %u/%zu top-1, %u/%zu in the 4 chunks the model sees\n", top1, G_N_ELEMENTS (cases), top4,
           G_N_ELEMENTS (cases));
  CHECK (top4 == G_N_ELEMENTS (cases), "the answer must reach the model: %u/%zu", top4, G_N_ELEMENTS (cases));
  CHECK (top1 >= 9, "top-1 accuracy regressed: %u/%zu", top1, G_N_ELEMENTS (cases));

  /* accents and case do not matter */
  CHECK (g_strcmp0 (top_file (ix, "GARANTIA", NULL), "manual.md") == 0, "unaccented upper-case query finds the manual");
  CHECK (g_strcmp0 (top_file (ix, "privacidad datos terceros", NULL), "politica.txt") == 0, "text file is searchable");

  /* only stop words, or unknown words: nothing comes back */
  g_autoptr (GPtrArray) none = rag_search (ix, "¿Cómo es el de la?", 4);
  CHECK (none->len == 0, "a query with no content words returns nothing");
  g_autoptr (GPtrArray) unknown = rag_search (ix, "xyzzy plugh", 4);
  CHECK (unknown->len == 0, "unknown words return nothing");

  /* weak matches are cut: asking about one thing should not return all of them */
  g_autoptr (GPtrArray) focused = rag_search (ix, "garantía defectos fabricación", 4);
  CHECK (focused->len >= 1 && focused->len <= 4, "focused query hit count is sane (%u)", focused->len);

  if (rag_pdf_available ())
    {
      int page = 0;
      const char *f = top_file (ix, "panel solar inversores", &page);
      CHECK (g_strcmp0 (f, "catalogo.pdf") == 0, "PDF text is searchable (got %s)", f ? f : "nothing");
      CHECK (page == 2, "PDF hit carries its page number (got %d)", page);
    }
  rag_index_free (ix);

  /* ---- removing a document ---- */
  guint manual_id = 0;
  for (guint i = 0; i < loaded->docs->len; i++)
    if (g_str_equal (((RagDoc *) loaded->docs->pdata[i])->name, "manual.md"))
      manual_id = ((RagDoc *) loaded->docs->pdata[i])->id;
  CHECK (manual_id != 0, "manual.md has an id");
  CHECK (rag_library_remove_doc (loaded, manual_id), "remove succeeds");
  g_ptr_array_set_size (selection, 0);
  g_ptr_array_add (selection, loaded);
  ix = rag_index_new (selection);
  g_autoptr (GPtrArray) gone = rag_search (ix, "garantía defectos fabricación", 4);
  CHECK (gone->len == 0, "removed document no longer appears (%u hits)", gone->len);
  CHECK (g_strcmp0 (top_file (ix, "privacidad datos terceros", NULL), "politica.txt") == 0, "other documents remain");
  rag_index_free (ix);

  /* ---- adding the same file again replaces it instead of duplicating ---- */
  char *again[] = { txt_path, NULL };
  guint before = loaded->docs->len;
  r = import (loaded, again);
  g_strfreev (r.errors);
  CHECK (loaded->docs->len == before, "re-adding replaces (%u -> %u)", before, loaded->docs->len);

  /* ---- deleting the library ---- */
  rag_library_delete (loaded);
  g_autoptr (GPtrArray) after = rag_libraries_load ();
  CHECK (after->len == 0, "library is gone from disk");

  rag_library_free (lib);
  rag_shutdown ();

  g_print (failures ? "test-rag: %d failure(s)\n" : "test-rag: all checks passed\n", failures);
  return failures ? 1 : 0;
}
