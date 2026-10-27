#pragma once

#include <gio/gio.h>

typedef struct ProcessWatch ProcessWatch;
typedef void (*ProcessWatchChanged)(ProcessWatch *watch, const GError *error, gpointer data);

ProcessWatch *process_watch_new(GHashTable *commands, gboolean force_poll,
                              ProcessWatchChanged changed, gpointer data, GError **error);
gboolean process_watch_set_commands(ProcessWatch *watch, GHashTable *commands, GError **error);
GHashTable *process_watch_active(ProcessWatch *watch); /* caller owns the returned set */
gboolean process_watch_running(ProcessWatch *watch);
const char *process_watch_mode(ProcessWatch *watch);
void process_watch_free(ProcessWatch *watch);
