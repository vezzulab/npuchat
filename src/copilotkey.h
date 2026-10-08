#pragma once

#include <gio/gio.h>

/* The Copilot key sends Shift+Super+F23, and keyboard layouts name that
 * symbol "Assistant", which KDE cannot bind to a shortcut. A one-line udev
 * hwdb rule makes the key send F19 instead; NPU Chat installs it through
 * polkit (pkexec) and then binds Meta+Shift+F19 to itself in KDE. */

gboolean copilotkey_supported (void);   /* KDE session with pkexec available */
gboolean copilotkey_installed (void);

typedef void (*CopilotKeyDone) (gboolean ok, gpointer data);

/* Asks for the administrator password, installs or removes the rule, then
 * sets or clears the KDE shortcut. done is always called, on the main loop. */
void copilotkey_set (gboolean enable, CopilotKeyDone done, gpointer data);
