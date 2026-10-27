#include "config.h"

#include <json-glib/json-glib.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

GHashTable *load_commands(const char *path, GError **error) {
    GHashTable *names = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_file(parser, path, error)) {
        if (g_error_matches(*error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
            g_clear_error(error);
            g_hash_table_add(names, g_strdup("rustc"));
            g_hash_table_add(names, g_strdup("cargo"));
            g_object_unref(parser);
            return names;
        }
        goto fail;
    }
    JsonNode *root = json_parser_get_root(parser);
    JsonObject *obj = JSON_NODE_HOLDS_OBJECT(root) ? json_node_get_object(root) : NULL;
    JsonNode *node = obj ? json_object_get_member(obj, "commands") : NULL;
    if (!node || !JSON_NODE_HOLDS_ARRAY(node)) {
        g_set_error_literal(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "commands must be an array of process names");
        goto fail;
    }
    JsonArray *array = json_node_get_array(node);
    for (guint i = 0; i < json_array_get_length(array); i++) {
        JsonNode *element = json_array_get_element(array, i);
        if (!JSON_NODE_HOLDS_VALUE(element) || json_node_get_value_type(element) != G_TYPE_STRING) {
            g_set_error_literal(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "commands must contain strings");
            goto fail;
        }
        const char *name = json_node_get_string(element);
        if (!name || !*name || strlen(name) > 15 || strchr(name, '/') || strchr(name, '\n')) {
            g_set_error_literal(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "process names must be 1–15 bytes without slashes or newlines");
            goto fail;
        }
        g_hash_table_add(names, g_strdup(name));
    }
    g_object_unref(parser);
    return names;
fail:
    g_object_unref(parser);
    g_hash_table_unref(names);
    return NULL;
}

int edit_command(const char *action, const char *name) {
    GError *error = NULL;
    if (!name || !*name || strlen(name) > 15 || strchr(name, '/') || strchr(name, '\n') || !g_utf8_validate(name, -1, NULL)) {
        g_printerr("omapower: invalid command name\n");
        return 1;
    }
    gchar *dir = g_build_filename(g_get_user_config_dir(), "omapower", NULL);
    gchar *path = g_build_filename(dir, "config.json", NULL);
    gchar *lockpath = g_build_filename(dir, "config.lock", NULL);
    int fd = -1, rc = 1;
    GHashTable *names = NULL;
    JsonParser *parser = NULL;
    JsonNode *root = NULL;
    JsonGenerator *generator = NULL;
    gchar *output = NULL;
    if (g_mkdir_with_parents(dir, 0700) != 0 || (fd = open(lockpath, O_CREAT | O_RDWR | O_CLOEXEC, 0600)) < 0 || flock(fd, LOCK_EX) != 0) {
        g_printerr("omapower: config lock: %s\n", g_strerror(errno));
        goto done;
    }
    names = load_commands(path, &error);
    if (!names) goto failure;
    parser = json_parser_new();
    if (json_parser_load_from_file(parser, path, &error)) {
        root = json_node_copy(json_parser_get_root(parser));
    } else if (g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
        g_clear_error(&error);
        root = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(root, json_object_new());
    } else goto failure;
    if (strcmp(action, "--add") == 0) g_hash_table_add(names, g_strdup(name));
    else g_hash_table_remove(names, name);
    GList *keys = g_hash_table_get_keys(names);
    keys = g_list_sort(keys, (GCompareFunc)g_strcmp0);
    JsonArray *array = json_array_new();
    for (GList *p = keys; p; p = p->next) json_array_add_string_element(array, p->data);
    g_list_free(keys);
    json_object_set_array_member(json_node_get_object(root), "commands", array);
    generator = json_generator_new();
    json_generator_set_pretty(generator, TRUE);
    json_generator_set_root(generator, root);
    output = json_generator_to_data(generator, NULL);
    if (!g_file_set_contents(path, output, -1, &error)) goto failure;
    rc = 0;
    goto done;
failure:
    g_printerr("omapower: %s\n", error ? error->message : "cannot update config");
    g_clear_error(&error);
done:
    g_free(output);
    if (generator) g_object_unref(generator);
    if (root) json_node_free(root);
    if (parser) g_object_unref(parser);
    if (names) g_hash_table_unref(names);
    if (fd >= 0) close(fd);
    g_free(lockpath); g_free(path); g_free(dir);
    return rc;
}

struct ConfigWatch {
    gchar *path;
    GFile *file, *directory, *target;
    GFileMonitor *monitor, *parent, *target_monitor;
    guint reload_source;
    gboolean rearm;
    ConfigChanged changed;
    gpointer data;
};

static void changed(GFileMonitor *monitor, GFile *file, GFile *other,
                    GFileMonitorEvent event, gpointer data);

static gchar *resolve_target(const char *path) {
    gchar *target = g_canonicalize_filename(path, NULL);
    for (int depth = 0; depth < 40; depth++) {
        gchar *physical = realpath(target, NULL);
        if (physical) { g_free(target); return physical; }
        /* realpath needs an existing final file. Follow dangling links too so
         * a later target creation is noticed without polling. */
        gchar *link = g_file_read_link(target, NULL);
        if (!link) break;
        gchar *dir = g_path_get_dirname(target);
        gchar *next = g_canonicalize_filename(link, dir);
        g_free(dir); g_free(link); g_free(target);
        target = next;
    }
    return target;
}

static gboolean arm(ConfigWatch *watch, GError **error) {
    if (watch->monitor) {
        g_file_monitor_cancel(watch->monitor);
        g_clear_object(&watch->monitor);
    }
    if (watch->target_monitor) g_file_monitor_cancel(watch->target_monitor);
    g_clear_object(&watch->target_monitor);
    g_clear_object(&watch->target);
    if (g_file_query_file_type(watch->directory, G_FILE_QUERY_INFO_NONE, NULL) != G_FILE_TYPE_DIRECTORY)
        return TRUE; /* parent monitor detects a later creation */
    watch->monitor = g_file_monitor_directory(watch->directory, G_FILE_MONITOR_WATCH_MOVES, NULL, error);
    if (!watch->monitor) return FALSE;
    g_signal_connect(watch->monitor, "changed", G_CALLBACK(changed), watch);
    gchar *resolved = resolve_target(watch->path);
    watch->target = g_file_new_for_path(resolved);
    g_free(resolved);
    if (!g_file_equal(watch->target, watch->file)) {
        GFile *dir = g_file_get_parent(watch->target);
        while (dir && g_file_query_file_type(dir, G_FILE_QUERY_INFO_NONE, NULL) != G_FILE_TYPE_DIRECTORY) {
            GFile *parent = g_file_get_parent(dir);
            g_object_unref(dir);
            dir = parent;
        }
        if (dir) {
            watch->target_monitor = g_file_monitor_directory(dir, G_FILE_MONITOR_WATCH_MOVES, NULL, error);
            g_object_unref(dir);
            if (!watch->target_monitor) return FALSE;
            g_signal_connect(watch->target_monitor, "changed", G_CALLBACK(changed), watch);
        }
    }
    return TRUE;
}

static gboolean reload(gpointer data) {
    ConfigWatch *watch = data;
    watch->reload_source = 0;
    GError *error = NULL;
    if (watch->rearm) {
        watch->rearm = FALSE;
        arm(watch, &error);
    }
    watch->changed(watch->path, error, watch->data);
    g_clear_error(&error);
    return G_SOURCE_REMOVE;
}

static void changed(GFileMonitor *monitor, GFile *file, GFile *other,
                    GFileMonitorEvent event, gpointer data) {
    (void)event;
    ConfigWatch *watch = data;
    GFile *target = monitor == watch->parent ? watch->directory
        : monitor == watch->target_monitor ? watch->target : watch->file;
    gboolean directory_event = (file && g_file_equal(file, watch->directory))
        || (monitor == watch->target_monitor && ((file && g_file_has_prefix(target, file))
                                                || (other && g_file_has_prefix(target, other))));
    if (!(file && g_file_equal(file, target)) && !(other && g_file_equal(other, target)) && !directory_event) return;
    watch->rearm = TRUE;
    if (watch->reload_source) g_source_remove(watch->reload_source);
    watch->reload_source = g_timeout_add(100, reload, watch);
}

ConfigWatch *config_watch_new(const char *path, ConfigChanged callback, gpointer data, GError **error) {
    ConfigWatch *watch = g_new0(ConfigWatch, 1);
    watch->path = g_strdup(path);
    watch->changed = callback;
    watch->data = data;
    gchar *dir = g_path_get_dirname(path);
    if (g_mkdir_with_parents(dir, 0700) != 0) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                    "create config directory: %s", g_strerror(errno));
        g_free(dir);
        config_watch_free(watch);
        return NULL;
    }
    watch->directory = g_file_new_for_path(dir);
    watch->file = g_file_new_for_path(path);
    g_free(dir);
    GFile *parent = g_file_get_parent(watch->directory);
    watch->parent = g_file_monitor_directory(parent, G_FILE_MONITOR_WATCH_MOVES, NULL, error);
    g_object_unref(parent);
    if (!watch->parent) { config_watch_free(watch); return NULL; }
    g_signal_connect(watch->parent, "changed", G_CALLBACK(changed), watch);
    if (!arm(watch, error)) { config_watch_free(watch); return NULL; }
    return watch;
}

void config_watch_free(ConfigWatch *watch) {
    if (!watch) return;
    if (watch->reload_source) g_source_remove(watch->reload_source);
    if (watch->monitor) g_file_monitor_cancel(watch->monitor);
    if (watch->parent) g_file_monitor_cancel(watch->parent);
    if (watch->target_monitor) g_file_monitor_cancel(watch->target_monitor);
    g_clear_object(&watch->target_monitor);
    g_clear_object(&watch->target);
    g_clear_object(&watch->monitor);
    g_clear_object(&watch->parent);
    g_clear_object(&watch->file);
    g_clear_object(&watch->directory);
    g_free(watch->path);
    g_free(watch);
}
