#pragma once

#include <glib.h>

/* Checks GitHub Releases for a newer version and, when running as an
 * AppImage, downloads it, verifies its SHA-256 and replaces the file. */

typedef struct {
  char *version;      /* "0.4.0" */
  char *page_url;     /* release page */
  char *appimage_url; /* NULL if the release has no AppImage */
  char *sha256_url;
} UpdateInfo;

typedef void (*UpdateCheckCb) (UpdateInfo *info, const char *error, gpointer data);
typedef void (*UpdateProgressCb) (double fraction, gpointer data);
typedef void (*UpdateDoneCb) (gboolean ok, const char *message, gpointer data);

/* info is NULL when already up to date (and error is NULL). The callback
 * owns info; free it with update_info_free(). */
void     updater_check (UpdateCheckCb cb, gpointer data);
gboolean updater_can_self_update (void);
void     updater_install (const UpdateInfo *info, UpdateProgressCb progress, UpdateDoneCb done, gpointer data);
/* Starts the (new) AppImage a moment after this process exits. */
void     updater_restart_after_exit (void);
void     update_info_free (UpdateInfo *info);
void     updater_shutdown (void);
