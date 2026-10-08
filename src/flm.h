#pragma once

#include <gio/gio.h>
#include <libsoup/soup.h>

typedef enum {
  FLM_STOPPED,
  FLM_LOADING,
  FLM_READY,
  FLM_ERROR,
} FlmState;

typedef void (*FlmStateCb) (FlmState state, const char *message, gpointer data);
typedef void (*FlmBoolCb) (gboolean ok, gpointer data);
typedef struct {
  char  *name;
  double footprint_gb; /* memory needed when loaded, 0 if unknown */
  double params_b;     /* billions of parameters, 0 if unknown */
  GStrv  labels;       /* e.g. "vision", "reasoning"; NULL if unknown */
} FlmModel;

/* models holds FlmModel* and is owned by the caller of the callback. */
typedef void (*FlmListCb) (GPtrArray *models, const char *error, gpointer data);
typedef void (*FlmProgressCb) (const char *line, double fraction, gpointer data);
typedef void (*FlmDoneCb) (gboolean ok, const char *message, gpointer data);

gboolean     flm_available (void);
SoupSession *flm_session (void);
char        *flm_url (const char *path);

/* Server lifecycle. If a server is already listening when flm_detect() runs,
 * it is used as is ("external") and never stopped by us. */
void     flm_init (FlmStateCb cb, gpointer data);
void     flm_detect (FlmBoolCb cb, gpointer data);
gboolean flm_is_external (void);
FlmState flm_state (void);
void     flm_load (const char *model, const char *pmode);
void     flm_stop (void);
const char *flm_loaded_model (void);
const char *flm_loaded_pmode (void);
/* Stops our server and frees all module state (call at exit). */
void     flm_shutdown (void);

/* Model management through the flm CLI. */
void flm_model_free (gpointer model);
void flm_list_models (const char *filter, FlmListCb cb, gpointer data);
void flm_pull (const char *model, FlmProgressCb progress, FlmDoneCb done, gpointer data);
void flm_remove (const char *model, FlmDoneCb done, gpointer data);