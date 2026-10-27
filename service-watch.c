#include "service-watch.h"
#include "config.h"

#include <json-glib/json-glib.h>

#include <gio/gio.h>
#include <glib-unix.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

#define SYSTEMD "org.freedesktop.systemd1"
#define MANAGER "/org/freedesktop/systemd1"
#define UNIT_INTERFACE "org.freedesktop.systemd1.Unit"
#define UNIT "omapower.service"

typedef struct {
    GDBusConnection *bus;
    GMainLoop *loop;
    gchar *owner, *path, *state, *last_config;
    ConfigWatch *config;
    gboolean referenced, stopping;
    int exit_status;
} ServiceWatch;

static void emit_state(ServiceWatch *watch, const char *state) {
    /* Only emit known enum values: the output is JSON, not shell text. */
    const char *values[] = { "active", "inactive", "failed", "activating", "deactivating",
                             "reloading", "maintenance", "refreshing", NULL };
    gboolean known = FALSE;
    for (int i = 0; values[i]; i++) if (!g_strcmp0(state, values[i])) known = TRUE;
    if (!known) state = "unknown";
    if (!g_strcmp0(state, watch->state)) return;
    g_free(watch->state);
    watch->state = g_strdup(state);
    g_print("{\"service\":\"%s\"}\n", state);
    if (fflush(stdout) != 0) {
        watch->exit_status = 1;
        g_main_loop_quit(watch->loop);
    }
}

static void config_changed(const char *path, const GError *monitor_error, gpointer data) {
    ServiceWatch *watch = data;
    if (monitor_error) {
        g_printerr("omapower: widget monitor: %s\n", monitor_error->message);
        watch->exit_status = 1;
        g_main_loop_quit(watch->loop);
        return;
    }
    GError *error = NULL;
    GHashTable *commands = load_commands(path, &error);
    JsonBuilder *builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "configError");
    json_builder_add_boolean_value(builder, commands == NULL);
    json_builder_set_member_name(builder, "commands");
    json_builder_begin_array(builder);
    if (commands) {
        GList *keys = g_list_sort(g_hash_table_get_keys(commands), (GCompareFunc)g_strcmp0);
        for (GList *p = keys; p; p = p->next) json_builder_add_string_value(builder, p->data);
        g_list_free(keys);
        g_hash_table_unref(commands);
    }
    json_builder_end_array(builder);
    json_builder_end_object(builder);
    JsonNode *root = json_builder_get_root(builder);
    gchar *json = json_to_string(root, FALSE);
    if (g_strcmp0(json, watch->last_config)) {
        g_print("%s\n", json);
        if (fflush(stdout) != 0) {
            watch->exit_status = 1;
            g_main_loop_quit(watch->loop);
        }
    }
    g_free(watch->last_config);
    watch->last_config = json;
    json_node_free(root);
    g_object_unref(builder);
    g_clear_error(&error); /* invalid configuration is displayed, not fatal to the widget */
}

static GVariant *call(ServiceWatch *watch, const char *path, const char *interface,
                      const char *method, GVariant *parameters, const GVariantType *type) {
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_sync(watch->bus, watch->owner, path, interface,
        method, parameters, type, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);
    if (!reply) {
        g_printerr("omapower: service monitor: %s\n", error->message);
        g_error_free(error);
    }
    return reply;
}

static void refresh(ServiceWatch *watch, gboolean load) {
    if (!watch->owner) return;
    GVariant *reply;
    if (load || !watch->path) {
        reply = call(watch, MANAGER, SYSTEMD ".Manager", "LoadUnit", g_variant_new("(s)", UNIT), G_VARIANT_TYPE("(o)"));
        if (!reply) { emit_state(watch, "unknown"); return; }
        g_free(watch->path);
        g_variant_get(reply, "(o)", &watch->path);
        g_variant_unref(reply);
    }
    if (!watch->referenced) {
        /* Pin the unit while observing it. Otherwise inactive units can be
         * garbage-collected, causing a UnitRemoved/LoadUnit event loop. The
         * reference is automatically dropped when this connection closes. */
        reply = call(watch, MANAGER, SYSTEMD ".Manager", "RefUnit", g_variant_new("(s)", UNIT), G_VARIANT_TYPE_UNIT);
        if (!reply) { emit_state(watch, "unknown"); return; }
        g_variant_unref(reply);
        watch->referenced = TRUE;
    }
    reply = call(watch, watch->path, "org.freedesktop.DBus.Properties", "GetAll",
                 g_variant_new("(s)", UNIT_INTERFACE), G_VARIANT_TYPE("(a{sv})"));
    if (!reply) { emit_state(watch, "unknown"); return; }
    GVariant *properties;
    g_variant_get(reply, "(@a{sv})", &properties);
    const char *load_state = NULL, *active = NULL;
    g_variant_lookup(properties, "LoadState", "&s", &load_state);
    g_variant_lookup(properties, "ActiveState", "&s", &active);
    emit_state(watch, !g_strcmp0(load_state, "loaded") ? active : "unknown");
    g_variant_unref(properties);
    g_variant_unref(reply);
}

static void appeared(GDBusConnection *connection, const gchar *name, const gchar *owner, gpointer data) {
    (void)connection; (void)name;
    ServiceWatch *watch = data;
    g_free(watch->owner);
    watch->owner = g_strdup(owner);
    watch->referenced = FALSE;
    GVariant *reply = call(watch, MANAGER, SYSTEMD ".Manager", "Subscribe", NULL, G_VARIANT_TYPE_UNIT);
    if (!reply) {
        watch->exit_status = 1;
        g_main_loop_quit(watch->loop);
        return;
    }
    g_variant_unref(reply);
    refresh(watch, TRUE);
}

static void vanished(GDBusConnection *connection, const gchar *name, gpointer data) {
    (void)connection; (void)name;
    ServiceWatch *watch = data;
    g_clear_pointer(&watch->owner, g_free);
    g_clear_pointer(&watch->path, g_free);
    watch->referenced = FALSE;
    emit_state(watch, "unknown");
}

static void changed(GDBusConnection *connection, const gchar *sender, const gchar *path,
                    const gchar *interface, const gchar *member, GVariant *parameters, gpointer data) {
    (void)connection;
    ServiceWatch *watch = data;
    if (g_strcmp0(sender, watch->owner)) return;
    if (!strcmp(interface, "org.freedesktop.DBus.Properties")) {
        if (watch->path && !strcmp(path, watch->path)) refresh(watch, FALSE);
    } else if (!strcmp(member, "Reloading") && g_variant_is_of_type(parameters, G_VARIANT_TYPE("(b)"))) {
        gboolean reloading;
        g_variant_get(parameters, "(b)", &reloading);
        if (!reloading) refresh(watch, TRUE);
    } else if ((!strcmp(member, "UnitNew") || !strcmp(member, "UnitRemoved"))
               && g_variant_is_of_type(parameters, G_VARIANT_TYPE("(so)"))) {
        const char *unit, *unit_path;
        g_variant_get(parameters, "(&s&o)", &unit, &unit_path);
        if (strcmp(unit, UNIT)) return;
        if (!strcmp(member, "UnitRemoved")) {
            g_clear_pointer(&watch->path, g_free);
            watch->referenced = FALSE;
            emit_state(watch, "unknown");
        } else refresh(watch, TRUE);
    }
}

static gboolean stop(gpointer data) {
    g_main_loop_quit(((ServiceWatch *)data)->loop);
    return G_SOURCE_CONTINUE;
}

static void closed(GDBusConnection *bus, gboolean remote, GError *error, gpointer data) {
    (void)bus; (void)remote; (void)error;
    ServiceWatch *watch = data;
    if (!watch->stopping) {
        watch->exit_status = 1;
        g_main_loop_quit(watch->loop);
    }
}

int watch_widget(void) {
    ServiceWatch watch = {0};
    watch.loop = g_main_loop_new(NULL, FALSE);
    GError *error = NULL;
    gchar *config_path = g_build_filename(g_get_user_config_dir(), "omapower", "config.json", NULL);
    watch.config = config_watch_new(config_path, config_changed, &watch, &error);
    if (watch.config) watch.bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (!watch.bus) {
        g_printerr("omapower: service monitor: %s\n", error->message);
        g_error_free(error);
        config_watch_free(watch.config);
        g_free(config_path);
        g_main_loop_unref(watch.loop);
        return 1;
    }
    g_dbus_connection_set_exit_on_close(watch.bus, FALSE);
    g_signal_connect(watch.bus, "closed", G_CALLBACK(closed), &watch);
    guint properties = g_dbus_connection_signal_subscribe(watch.bus, SYSTEMD, "org.freedesktop.DBus.Properties",
        "PropertiesChanged", NULL, UNIT_INTERFACE, G_DBUS_SIGNAL_FLAGS_NONE, changed, &watch, NULL);
    guint manager = g_dbus_connection_signal_subscribe(watch.bus, SYSTEMD, SYSTEMD ".Manager",
        NULL, MANAGER, NULL, G_DBUS_SIGNAL_FLAGS_NONE, changed, &watch, NULL);
    guint name = g_bus_watch_name_on_connection(watch.bus, SYSTEMD, G_BUS_NAME_WATCHER_FLAGS_NONE,
                                               appeared, vanished, &watch, NULL);
    guint term = g_unix_signal_add(SIGTERM, stop, &watch);
    guint interrupt = g_unix_signal_add(SIGINT, stop, &watch);
    emit_state(&watch, "unknown");
    config_changed(config_path, NULL, &watch);
    g_free(config_path);
    if (!watch.exit_status) g_main_loop_run(watch.loop);
    watch.stopping = TRUE;
    g_source_remove(term);
    g_source_remove(interrupt);
    config_watch_free(watch.config);
    g_bus_unwatch_name(name);
    g_dbus_connection_signal_unsubscribe(watch.bus, properties);
    g_dbus_connection_signal_unsubscribe(watch.bus, manager);
    g_dbus_connection_close_sync(watch.bus, NULL, NULL);
    g_object_unref(watch.bus);
    g_main_loop_unref(watch.loop);
    g_free(watch.owner); g_free(watch.path); g_free(watch.state); g_free(watch.last_config);
    return watch.exit_status;
}
