#include <glib/gstdio.h>
#include "../config.h"

static void test_config(void) {
    GError *error = NULL;
    gchar *dir = g_dir_make_tmp("omapower-test-XXXXXX", &error);
    g_assert_no_error(error);
    gchar *file = g_build_filename(dir, "config.json", NULL);
    GHashTable *names = load_commands(file, &error);
    g_assert_no_error(error);
    g_assert_true(g_hash_table_contains(names, "rustc"));
    g_assert_true(g_hash_table_contains(names, "cargo"));
    g_hash_table_unref(names);

    g_assert_true(g_file_set_contents(file, "{\"commands\":[\"make\"]}", -1, &error));
    names = load_commands(file, &error);
    g_assert_no_error(error);
    g_assert_true(g_hash_table_contains(names, "make"));
    g_assert_false(g_hash_table_contains(names, "rustc"));
    g_hash_table_unref(names);

    g_assert_true(g_file_set_contents(file, "{\"commands\":[\"/bin/rustc\"]}", -1, &error));
    names = load_commands(file, &error);
    g_assert_null(names);
    g_assert_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL);
    g_clear_error(&error);
    g_remove(file);
    g_rmdir(dir);
    g_free(file); g_free(dir);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/omapower/config", test_config);
    return g_test_run();
}
