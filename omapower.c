#include <gio/gio.h>
#include <glib-unix.h>
#include <json-glib/json-glib.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "process-watch.h"
#include "service-watch.h"

#define DEST "net.hadess.PowerProfiles"
#define OBJECT "/net/hadess/PowerProfiles"

typedef enum { IDLE, ACQUIRING, RELEASING } Operation;

typedef struct {
    GDBusConnection *bus;
    GMainLoop *loop;
    ProcessWatch *processes;
    ConfigWatch *config_watch;
    gchar *config, *status, *last_status, *owner;
    GCancellable *cancel;
    guint generation, cookie, released_cookie, name_watch, subscription;
    guint reconcile_source, retry_source;
    guint release_failures;
    Operation pending;
    gboolean holding, suppressed, stopping, release_seen;
    int exit_status;
} App;

static void reconcile(App *app);

static void remove_source(guint *source) {
    if (*source) g_source_remove(*source);
    *source = 0;
}

static void fail(App *app, const char *message) {
    g_printerr("omapower: %s\n", message);
    app->exit_status = 1;
    g_main_loop_quit(app->loop);
}

static gboolean write_status(App *app, GError **error) {
    GHashTable *active = process_watch_active(app->processes);
    GList *keys = g_list_sort(g_hash_table_get_keys(active), (GCompareFunc)g_strcmp0);
    JsonBuilder *builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "active");
    json_builder_begin_array(builder);
    for (GList *p = keys; p; p = p->next) json_builder_add_string_value(builder, p->data);
    json_builder_end_array(builder);
    json_builder_set_member_name(builder, "holding"); json_builder_add_boolean_value(builder, app->holding);
    json_builder_set_member_name(builder, "suppressed"); json_builder_add_boolean_value(builder, app->suppressed);
    json_builder_set_member_name(builder, "available"); json_builder_add_boolean_value(builder, app->owner != NULL);
    json_builder_set_member_name(builder, "monitor"); json_builder_add_string_value(builder, process_watch_mode(app->processes));
    json_builder_end_object(builder);
    JsonNode *root = json_builder_get_root(builder);
    gchar *json = json_to_string(root, FALSE);
    gboolean written = !g_strcmp0(json, app->last_status) || g_file_set_contents(app->status, json, -1, error);
    if (written) { g_free(app->last_status); app->last_status = json; }
    else g_free(json);
    json_node_free(root);
    g_object_unref(builder);
    g_list_free(keys);
    g_hash_table_unref(active);
    return written;
}

static gboolean reconcile_idle(gpointer data) {
    App *app = data;
    app->reconcile_source = 0;
    reconcile(app);
    return G_SOURCE_REMOVE;
}

static void schedule_reconcile(App *app) {
    if (!app->stopping && !app->reconcile_source)
        app->reconcile_source = g_idle_add_full(G_PRIORITY_DEFAULT, reconcile_idle, app, NULL);
}

static gboolean retry_release(gpointer data) {
    App *app = data;
    app->retry_source = 0;
    schedule_reconcile(app);
    return G_SOURCE_REMOVE;
}

typedef struct { App *app; guint generation; Operation operation; } PendingCall;

static void call_finished(GObject *connection, GAsyncResult *result, gpointer data) {
    PendingCall *call = data;
    App *app = call->app;
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(connection), result, &error);
    app->pending = IDLE;
    g_clear_object(&app->cancel);
    if (!app->stopping && call->generation == app->generation) {
        if (reply) {
            if (call->operation == ACQUIRING) {
                g_variant_get(reply, "(u)", &app->cookie);
                app->holding = !(app->release_seen && app->released_cookie == app->cookie);
                if (!app->holding) {
                    app->cookie = 0;
                    app->suppressed = process_watch_running(app->processes);
                }
            } else {
                app->holding = FALSE;
                app->cookie = 0;
                app->release_failures = 0;
            }
        } else if (call->operation == RELEASING && app->holding) {
            /* A failed/ambiguous release is NOT a successful release. Retain
             * the cookie; disconnect after bounded retries to drop all holds. */
            g_printerr("omapower: release: %s\n", error->message);
            if (++app->release_failures >= 3) fail(app, "release failed three times; disconnecting to drop the hold");
            else app->retry_source = g_timeout_add_seconds(app->release_failures, retry_release, app);
        } else if (call->operation == ACQUIRING) {
            /* A timed-out acquisition may still have succeeded remotely.
             * Never retry it on the same connection and leak another cookie. */
            fail(app, error->message);
        }
    }
    if (reply) g_variant_unref(reply);
    g_clear_error(&error);
    g_free(call);
    schedule_reconcile(app);
}

static void start_call(App *app, Operation operation) {
    app->pending = operation;
    if (operation == ACQUIRING) {
        app->release_seen = FALSE;
        app->release_failures = 0;
    }
    app->cancel = g_cancellable_new();
    PendingCall *call = g_new(PendingCall, 1);
    *call = (PendingCall){ app, app->generation, operation };
    g_dbus_connection_call(app->bus, app->owner, OBJECT, DEST,
        operation == ACQUIRING ? "HoldProfile" : "ReleaseProfile",
        operation == ACQUIRING ? g_variant_new("(sss)", "performance", "Whitelisted command running", "omapower")
                              : g_variant_new("(u)", app->cookie),
        operation == ACQUIRING ? G_VARIANT_TYPE("(u)") : G_VARIANT_TYPE_UNIT,
        G_DBUS_CALL_FLAGS_NONE, 5000, app->cancel, call_finished, call);
}

static void reconcile(App *app) {
    gboolean running = process_watch_running(app->processes);
    if (!running) app->suppressed = FALSE;
    gboolean wanted = running && !app->suppressed;
    if (wanted && app->holding && app->retry_source) {
        remove_source(&app->retry_source);
        app->release_failures = 0;
    }
    if (app->pending == IDLE && app->owner && !app->retry_source) {
        if (wanted && !app->holding) start_call(app, ACQUIRING);
        else if (!wanted && app->holding) start_call(app, RELEASING);
    }
    GError *error = NULL;
    if (!write_status(app, &error)) {
        fail(app, error->message);
        g_error_free(error);
    }
}

static void processes_changed(ProcessWatch *watch, const GError *error, gpointer data) {
    (void)watch;
    App *app = data;
    if (error) fail(app, error->message);
    else schedule_reconcile(app);
}

static void owner_changed(App *app, const char *owner) {
    if (app->stopping || !g_strcmp0(app->owner, owner)) return;
    g_free(app->owner);
    app->owner = g_strdup(owner);
    app->generation++;
    app->holding = FALSE;
    app->cookie = 0;
    app->release_failures = 0;
    remove_source(&app->retry_source);
    if (app->cancel) g_cancellable_cancel(app->cancel);
    schedule_reconcile(app);
}

static void appeared(GDBusConnection *connection, const gchar *name, const gchar *owner, gpointer data) {
    (void)connection; (void)name;
    owner_changed(data, owner);
}

static void vanished(GDBusConnection *connection, const gchar *name, gpointer data) {
    (void)connection; (void)name;
    owner_changed(data, NULL);
}

static void bus_closed(GDBusConnection *connection, gboolean remote, GError *error, gpointer data) {
    (void)connection; (void)remote;
    App *app = data;
    if (!app->stopping) fail(app, error ? error->message : "system bus disconnected");
}

static void profile_released(GDBusConnection *connection, const gchar *sender, const gchar *path,
                             const gchar *interface, const gchar *signal_name,
                             GVariant *parameters, gpointer data) {
    (void)connection; (void)path; (void)interface; (void)signal_name;
    App *app = data;
    if (app->stopping || g_strcmp0(sender, app->owner)
        || !g_variant_is_of_type(parameters, G_VARIANT_TYPE("(u)"))) return;
    guint cookie;
    g_variant_get(parameters, "(u)", &cookie);
    if (app->pending == ACQUIRING) {
        app->released_cookie = cookie;
        app->release_seen = TRUE;
    }
    if (app->holding && app->cookie == cookie) {
        app->holding = FALSE;
        app->cookie = 0;
        if (app->pending != RELEASING) app->suppressed = process_watch_running(app->processes);
        remove_source(&app->retry_source);
        schedule_reconcile(app);
    }
}

static void config_changed(const char *path, const GError *monitor_error, gpointer data) {
    App *app = data;
    if (app->stopping) return;
    if (monitor_error) { fail(app, monitor_error->message); return; }
    GError *error = NULL;
    GHashTable *commands = load_commands(path, &error);
    if (commands) {
        process_watch_set_commands(app->processes, commands, &error);
        g_hash_table_unref(commands);
    }
    if (error) { fail(app, error->message); g_error_free(error); }
    else schedule_reconcile(app);
}

static gboolean stop(gpointer data) {
    g_main_loop_quit(((App *)data)->loop);
    return G_SOURCE_CONTINUE; /* cleanup removes the signal sources */
}

int main(int argc, char **argv) {
    if (argc == 3 && (!strcmp(argv[1], "--add") || !strcmp(argv[1], "--remove")))
        return edit_command(argv[1], argv[2]);
    if (argc == 2 && !strcmp(argv[1], "--watch-widget")) return watch_widget();
    gboolean force_poll = argc == 2 && !strcmp(argv[1], "--poll");
    if (argc != 1 && !force_poll) {
        g_printerr("Usage: omapower [--poll | --watch-widget | --add NAME | --remove NAME]\n");
        return 2;
    }
    App app = {0};
    GError *error = NULL;
    guint term = 0, interrupt = 0;
    app.config = g_build_filename(g_get_user_config_dir(), "omapower", "config.json", NULL);
    app.status = g_build_filename(g_get_user_runtime_dir(), "omapower.json", NULL);
    app.loop = g_main_loop_new(NULL, FALSE);
    app.config_watch = config_watch_new(app.config, config_changed, &app, &error);
    if (!app.config_watch) goto failure;
    GHashTable *commands = load_commands(app.config, &error);
    if (!commands) goto failure;
    app.processes = process_watch_new(commands, force_poll, processes_changed, &app, &error);
    g_hash_table_unref(commands);
    if (!app.processes) goto failure;
    app.bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (!app.bus) goto failure;
    g_dbus_connection_set_exit_on_close(app.bus, FALSE);
    g_signal_connect(app.bus, "closed", G_CALLBACK(bus_closed), &app);
    app.subscription = g_dbus_connection_signal_subscribe(app.bus, DEST, DEST, "ProfileReleased", OBJECT,
        NULL, G_DBUS_SIGNAL_FLAGS_NONE, profile_released, &app, NULL);
    app.name_watch = g_bus_watch_name_on_connection(app.bus, DEST, G_BUS_NAME_WATCHER_FLAGS_AUTO_START,
                                                   appeared, vanished, &app, NULL);
    term = g_unix_signal_add(SIGTERM, stop, &app);
    interrupt = g_unix_signal_add(SIGINT, stop, &app);
    schedule_reconcile(&app);
    g_main_loop_run(app.loop);
    goto done;
failure:
    fail(&app, error ? error->message : "initialization failed");
    g_clear_error(&error);
done:
    app.stopping = TRUE;
    remove_source(&term);
    remove_source(&interrupt);
    remove_source(&app.reconcile_source);
    remove_source(&app.retry_source);
    config_watch_free(app.config_watch);
    process_watch_free(app.processes);
    if (app.name_watch) g_bus_unwatch_name(app.name_watch);
    if (app.subscription) g_dbus_connection_signal_unsubscribe(app.bus, app.subscription);
    if (app.cancel) g_cancellable_cancel(app.cancel);
    if (app.bus) {
        /* Holds belong to this connection. Closing it also cleans up a hold
         * whose acquisition/release reply was lost or cancelled. */
        g_dbus_connection_close_sync(app.bus, NULL, NULL);
        while (app.pending != IDLE) g_main_context_iteration(NULL, TRUE);
        g_object_unref(app.bus);
    }
    if (app.last_status) unlink(app.status);
    g_main_loop_unref(app.loop);
    g_free(app.config); g_free(app.status); g_free(app.last_status); g_free(app.owner);
    return app.exit_status;
}
