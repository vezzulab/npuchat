#include "net.h"

#include "selftest.h"

static SoupSession *session;

/* The AppImage bundles its own TLS backend but not a CA list, so point it at
 * the host's certificates (paths differ between distros). */
SoupSession *
net_session (void)
{
  if (session)
    return session;

  session = TRACK (soup_session_new_with_options ("timeout", 30, "user-agent",
                                                  "NPU-Chat/" NPU_CHAT_VERSION " (https://github.com/vezzulab/npuchat)",
                                                  NULL));
  static const char *ca_files[] = {
    "/etc/ssl/certs/ca-certificates.crt", /* Debian, Ubuntu, Arch */
    "/etc/pki/tls/certs/ca-bundle.crt",   /* Fedora, RHEL */
    "/etc/ssl/ca-bundle.pem",             /* openSUSE */
    "/etc/ssl/cert.pem",                  /* Alpine, others */
    NULL,
  };
  for (int i = 0; ca_files[i]; i++)
    if (g_file_test (ca_files[i], G_FILE_TEST_EXISTS))
      {
        g_autoptr (GTlsDatabase) db = g_tls_file_database_new (ca_files[i], NULL);
        if (db)
          {
            soup_session_set_tls_database (session, db);
            break;
          }
      }
  return session;
}

void
net_shutdown (void)
{
  g_clear_object (&session);
}
