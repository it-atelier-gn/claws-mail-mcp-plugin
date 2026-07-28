/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Georg Nelles */

#include <glib.h>
#include <string.h>

#include "mcp_json.h"

static void test_parse_object(void)
{
	GError *error = NULL;
	McpJson *root = mcp_json_parse("{\"a\":1,\"b\":\"x\",\"c\":true,\"d\":null}", -1, &error);

	g_assert_no_error(error);
	g_assert_nonnull(root);
	g_assert_true(mcp_json_is(root, MCP_JSON_OBJECT));
	g_assert_cmpint(mcp_json_member_int(root, "a", -1), ==, 1);
	g_assert_cmpstr(mcp_json_member_string(root, "b", NULL), ==, "x");
	g_assert_true(mcp_json_member_bool(root, "c", FALSE));
	g_assert_true(mcp_json_is(mcp_json_member(root, "d"), MCP_JSON_NULL));
	g_assert_null(mcp_json_member(root, "missing"));
	mcp_json_free(root);
}

static void test_parse_array(void)
{
	McpJson *root = mcp_json_parse("[1, \"two\", [3]]", -1, NULL);

	g_assert_nonnull(root);
	g_assert_cmpuint(mcp_json_length(root), ==, 3);
	g_assert_cmpint(mcp_json_get_int(mcp_json_index(root, 0), 0), ==, 1);
	g_assert_cmpstr(mcp_json_get_string(mcp_json_index(root, 1), NULL), ==, "two");
	g_assert_cmpuint(mcp_json_length(mcp_json_index(root, 2)), ==, 1);
	mcp_json_free(root);
}

static void test_escapes(void)
{
	McpJson *root = mcp_json_parse("\"a\\nb\\t\\\"c\\\"\\u00e4\\ud83d\\ude00\"", -1, NULL);

	g_assert_nonnull(root);
	g_assert_cmpstr(mcp_json_get_string(root, NULL), ==, "a\nb\t\"c\"\xc3\xa4\xf0\x9f\x98\x80");
	mcp_json_free(root);
}

static void test_negative_numbers(void)
{
	McpJson *root = mcp_json_parse("{\"n\":-42,\"f\":1.5e2}", -1, NULL);

	g_assert_cmpint(mcp_json_member_int(root, "n", 0), ==, -42);
	g_assert_cmpfloat(mcp_json_get_double(mcp_json_member(root, "f"), 0), ==, 150.0);
	mcp_json_free(root);
}

static void test_invalid(void)
{
	const gchar *bad[] = {
		"{", "[1,]", "{\"a\":}", "nul", "\"unterminated",
		"{\"a\":1} trailing", "'single'", "{\"a\"1}", ""
	};
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(bad); i++) {
		GError *error = NULL;
		McpJson *root = mcp_json_parse(bad[i], -1, &error);

		g_assert_null(root);
		g_assert_nonnull(error);
		g_clear_error(&error);
	}
}

static void test_depth_limit(void)
{
	GString *deep = g_string_new(NULL);
	McpJson *root;
	gint i;

	for (i = 0; i < 200; i++)
		g_string_append_c(deep, '[');
	for (i = 0; i < 200; i++)
		g_string_append_c(deep, ']');

	root = mcp_json_parse(deep->str, -1, NULL);
	g_assert_null(root);
	g_string_free(deep, TRUE);
}

static void test_roundtrip(void)
{
	const gchar *text = "{\"id\":7,\"nested\":{\"list\":[1,2,\"three\"]},\"flag\":false}";
	McpJson *root = mcp_json_parse(text, -1, NULL);
	gchar *again;

	g_assert_nonnull(root);
	again = mcp_json_to_string(root);
	g_assert_cmpstr(again, ==, text);
	g_free(again);
	mcp_json_free(root);
}

static void test_escape_output(void)
{
	gchar *quoted = mcp_json_quote("line\nbreak \"q\" \x01");

	g_assert_cmpstr(quoted, ==, "\"line\\nbreak \\\"q\\\" \\u0001\"");
	g_free(quoted);
}

static void test_string_id_is_preserved(void)
{
	McpJson *root = mcp_json_parse("{\"id\":\"call-1\"}", -1, NULL);
	GString *out = g_string_new(NULL);

	mcp_json_write(mcp_json_member(root, "id"), out);
	g_assert_cmpstr(out->str, ==, "\"call-1\"");
	g_string_free(out, TRUE);
	mcp_json_free(root);
}

static void test_copy_is_independent(void)
{
	McpJson *root = mcp_json_parse("{\"a\":[1,2,{\"b\":\"x\"}],\"c\":true}", -1, NULL);
	McpJson *copy;
	gchar *before, *after;

	g_assert_nonnull(root);
	copy = mcp_json_copy(root);
	g_assert_nonnull(copy);

	before = mcp_json_to_string(copy);
	mcp_json_free(root);
	after = mcp_json_to_string(copy);

	g_assert_cmpstr(before, ==, after);
	g_assert_cmpstr(after, ==, "{\"a\":[1,2,{\"b\":\"x\"}],\"c\":true}");

	g_free(before);
	g_free(after);
	mcp_json_free(copy);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/json/object", test_parse_object);
	g_test_add_func("/json/array", test_parse_array);
	g_test_add_func("/json/escapes", test_escapes);
	g_test_add_func("/json/numbers", test_negative_numbers);
	g_test_add_func("/json/invalid", test_invalid);
	g_test_add_func("/json/depth", test_depth_limit);
	g_test_add_func("/json/roundtrip", test_roundtrip);
	g_test_add_func("/json/escape-output", test_escape_output);
	g_test_add_func("/json/string-id", test_string_id_is_preserved);
	g_test_add_func("/json/copy", test_copy_is_independent);
	return g_test_run();
}
