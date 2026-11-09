/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Georg Nelles */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#ifndef G_OS_WIN32
#include <unistd.h>
#endif

#include "mcp_paths.h"

static gchar *make_root(void)
{
	gchar *root = g_dir_make_tmp("mcp-paths-XXXXXX", NULL);

	g_assert_nonnull(root);
	return root;
}

static void test_resolve_relative_inside(void)
{
	gchar *root = make_root();
	gchar *path = mcp_path_resolve(root, "out/mail.eml");
	gchar *expected = g_build_filename(root, "out", "mail.eml", NULL);
	gchar *canon = g_canonicalize_filename(expected, NULL);

	g_assert_cmpstr(path, ==, canon);
	g_free(canon);
	g_free(expected);
	g_free(path);
	g_rmdir(root);
	g_free(root);
}

static void test_resolve_absolute_inside(void)
{
	gchar *root = make_root();
	gchar *abs = g_build_filename(root, "a.txt", NULL);
	gchar *path = mcp_path_resolve(root, abs);

	g_assert_nonnull(path);
	g_free(path);
	g_free(abs);
	g_rmdir(root);
	g_free(root);
}

static void test_resolve_rejects_escape(void)
{
	gchar *root = make_root();
	gchar *sibling = g_strconcat(root, "-evil", G_DIR_SEPARATOR_S, "x.txt", NULL);
	gchar *parent = g_path_get_dirname(root);
	gchar *outside = g_build_filename(parent, "x.txt", NULL);

	g_assert_null(mcp_path_resolve(root, "../x.txt"));
	g_assert_null(mcp_path_resolve(root, "a/../../x.txt"));
	g_assert_null(mcp_path_resolve(root, "a\\..\\..\\x.txt"));
	g_assert_null(mcp_path_resolve(root, outside));
	g_assert_null(mcp_path_resolve(root, sibling));
	g_assert_null(mcp_path_resolve(root, root));
	g_assert_null(mcp_path_resolve(root, ""));
	g_assert_null(mcp_path_resolve(root, NULL));
	g_assert_null(mcp_path_resolve(NULL, "x"));

	g_free(outside);
	g_free(parent);
	g_free(sibling);
	g_rmdir(root);
	g_free(root);
}

#ifdef G_OS_WIN32
static void test_resolve_rejects_windows_tricks(void)
{
	gchar *root = make_root();

	g_assert_null(mcp_path_resolve(root, "file.txt:stream"));
	g_assert_null(mcp_path_resolve(root, "C:x.txt"));
	g_assert_null(mcp_path_resolve(root, "CON"));
	g_assert_null(mcp_path_resolve(root, "sub/nul.txt"));
	g_rmdir(root);
	g_free(root);
}
#else
static void test_resolve_rejects_symlink(void)
{
	gchar *root = make_root();
	gchar *link = g_build_filename(root, "link", NULL);

	g_assert_cmpint(symlink("/tmp", link), ==, 0);
	g_assert_null(mcp_path_resolve(root, "link/x.txt"));
	g_unlink(link);
	g_free(link);
	g_rmdir(root);
	g_free(root);
}
#endif

static void check_name(const gchar *raw, const gchar *expected)
{
	gchar *name = mcp_safe_filename(raw);

	g_assert_cmpstr(name, ==, expected);
	g_free(name);
}

static void test_safe_filename(void)
{
	gchar *long_raw, *name;

	check_name("report.pdf", "report.pdf");
	check_name("../../etc/passwd", "passwd");
	check_name("..\\..\\boot.ini", "boot.ini");
	check_name("a<b>c:d\"e|f?g*h.txt", "a_b_c_d_e_f_g_h.txt");
	check_name("tab\there.txt", "tab_here.txt");
	check_name("...", "attachment");
	check_name(" .hidden ", "hidden");
	check_name("trailing.", "trailing");
	check_name("CON.txt", "_CON.txt");
	check_name("lpt1", "_lpt1");
	check_name("console.txt", "console.txt");
	check_name("Rechnung \xc3\xa4.pdf", "Rechnung \xc3\xa4.pdf");
	check_name(NULL, "attachment");
	check_name("", "attachment");

	long_raw = g_strconcat(g_strnfill(300, 'x'), ".pdf", NULL);
	name = mcp_safe_filename(long_raw);
	g_assert_cmpuint(strlen(name), <=, 180);
	g_assert_true(g_str_has_suffix(name, ".pdf"));
	g_free(name);
	g_free(long_raw);
}

static void test_unique_path(void)
{
	gchar *root = make_root();
	gchar *first = mcp_unique_path(root, "a.txt");
	gchar *second, *third;

	g_assert_true(g_file_set_contents(first, "x", 1, NULL));
	second = mcp_unique_path(root, "a.txt");
	g_assert_true(g_str_has_suffix(second, "a-1.txt"));
	g_assert_true(g_file_set_contents(second, "x", 1, NULL));
	third = mcp_unique_path(root, "a.txt");
	g_assert_true(g_str_has_suffix(third, "a-2.txt"));

	g_unlink(first);
	g_unlink(second);
	g_free(first);
	g_free(second);
	g_free(third);
	g_rmdir(root);
	g_free(root);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/paths/relative", test_resolve_relative_inside);
	g_test_add_func("/paths/absolute", test_resolve_absolute_inside);
	g_test_add_func("/paths/escape", test_resolve_rejects_escape);
#ifdef G_OS_WIN32
	g_test_add_func("/paths/windows", test_resolve_rejects_windows_tricks);
#else
	g_test_add_func("/paths/symlink", test_resolve_rejects_symlink);
#endif
	g_test_add_func("/paths/safe-filename", test_safe_filename);
	g_test_add_func("/paths/unique", test_unique_path);
	return g_test_run();
}
