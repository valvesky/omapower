#pragma once

#include <gio/gio.h>

typedef struct ConfigWatch ConfigWatch;
typedef void (*ConfigChanged)(const char *path, const GError *error, gpointer data);

GHashTable *load_commands(const char *path, GError **error);
int edit_command(const char *action, const char *name);
ConfigWatch *config_watch_new(const char *path, ConfigChanged changed, gpointer data, GError **error);
void config_watch_free(ConfigWatch *watch);
