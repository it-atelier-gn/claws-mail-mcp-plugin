/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Georg Nelles */

#include <glib.h>
#include <string.h>

#include "mcp_msg_json.h"

static void init_msg(MsgInfo *msginfo, guint perm_flags, guint tmp_flags)
{
	memset(msginfo, 0, sizeof *msginfo);
	msginfo->msgnum = 7;
	msginfo->subject = "hello";
	msginfo->msgid = "abc@example.org";
	msginfo->inreplyto = "parent@example.org";
	msginfo->flags.perm_flags = perm_flags;
	msginfo->flags.tmp_flags = tmp_flags;
}

static McpJson *summarize(guint perm_flags, guint tmp_flags, const gchar *const *tags,
			  gboolean has_attachment)
{
	MsgInfo msginfo;
	GString *out = g_string_new(NULL);
	McpJson *root;

	init_msg(&msginfo, perm_flags, tmp_flags);
	append_msg_summary(out, &msginfo, "#mh/Mailbox/inbox", tags, has_attachment);
	root = mcp_json_parse(out->str, -1, NULL);
	g_string_free(out, TRUE);
	g_assert_nonnull(root);
	return root;
}

static void test_spam_flag_set(void)
{
	McpJson *root = summarize(MSG_SPAM, 0, NULL, FALSE);

	g_assert_true(mcp_json_member_bool(root, "spam", FALSE));
	g_assert_false(mcp_json_member_bool(root, "unread", TRUE));
	mcp_json_free(root);
}

static void test_spam_flag_clear(void)
{
	McpJson *root = summarize(MSG_UNREAD | MSG_MARKED, 0, NULL, FALSE);

	g_assert_false(mcp_json_member_bool(root, "spam", TRUE));
	g_assert_true(mcp_json_member_bool(root, "unread", FALSE));
	g_assert_true(mcp_json_member_bool(root, "marked", FALSE));
	mcp_json_free(root);
}

static void test_thread_and_lock_flags(void)
{
	McpJson *root = summarize(MSG_LOCKED | MSG_FORWARDED | MSG_WATCH_THREAD, MSG_SIGNED, NULL, TRUE);

	g_assert_true(mcp_json_member_bool(root, "locked", FALSE));
	g_assert_true(mcp_json_member_bool(root, "forwarded", FALSE));
	g_assert_true(mcp_json_member_bool(root, "watch_thread", FALSE));
	g_assert_false(mcp_json_member_bool(root, "ignore_thread", TRUE));
	g_assert_true(mcp_json_member_bool(root, "signed", FALSE));
	g_assert_false(mcp_json_member_bool(root, "encrypted", TRUE));
	g_assert_true(mcp_json_member_bool(root, "has_attachment", FALSE));
	mcp_json_free(root);
}

static void test_color_label(void)
{
	guint i;

	for (i = 0; i <= 15; i++) {
		McpJson *root = summarize(MSG_COLORLABEL_TO_FLAGS(i) | MSG_UNREAD, 0, NULL, FALSE);

		g_assert_cmpint(mcp_json_member_int(root, "color_label", -1), ==, (gint64)i);
		g_assert_true(mcp_json_member_bool(root, "unread", FALSE));
		mcp_json_free(root);
	}
}

static void test_tags_and_ids(void)
{
	const gchar *tags[] = { "Rechnung", "wichtig \"x\"", NULL };
	McpJson *root = summarize(0, 0, tags, FALSE);
	const McpJson *arr = mcp_json_member(root, "tags");

	g_assert_cmpuint(mcp_json_length(arr), ==, 2);
	g_assert_cmpstr(mcp_json_get_string(mcp_json_index(arr, 1), NULL), ==, "wichtig \"x\"");
	g_assert_cmpstr(mcp_json_member_string(root, "message_id", NULL), ==, "abc@example.org");
	g_assert_cmpstr(mcp_json_member_string(root, "in_reply_to", NULL), ==, "parent@example.org");
	mcp_json_free(root);

	root = summarize(0, 0, NULL, FALSE);
	g_assert_cmpuint(mcp_json_length(mcp_json_member(root, "tags")), ==, 0);
	mcp_json_free(root);
}

static void test_flag_names(void)
{
	guint bit = 0;

	g_assert_true(mcp_flag_from_name("spam", &bit));
	g_assert_cmpuint(bit, ==, MSG_SPAM);
	g_assert_true(mcp_flag_from_name("Watch_Thread", &bit));
	g_assert_cmpuint(bit, ==, MSG_WATCH_THREAD);
	g_assert_false(mcp_flag_from_name("deleted", &bit));
	g_assert_false(mcp_flag_from_name(NULL, &bit));
}

static McpMsgFilter parse_filter(const gchar *json)
{
	McpJson *args = mcp_json_parse(json, -1, NULL);
	McpMsgFilter f;
	gchar *error = NULL;

	g_assert_true(mcp_filter_parse(args, &f, &error));
	g_assert_null(error);
	mcp_json_free(args);
	return f;
}

static void test_filter_flags(void)
{
	MsgInfo spam, ham;
	McpMsgFilter f;

	init_msg(&spam, MSG_SPAM | MSG_UNREAD, 0);
	init_msg(&ham, 0, 0);

	f = parse_filter("{}");
	g_assert_true(mcp_filter_matches(&f, &spam, NULL));
	g_assert_true(mcp_filter_matches(&f, &ham, NULL));

	f = parse_filter("{\"spam\":true}");
	g_assert_true(mcp_filter_matches(&f, &spam, NULL));
	g_assert_false(mcp_filter_matches(&f, &ham, NULL));

	f = parse_filter("{\"spam\":false,\"unread\":false}");
	g_assert_false(mcp_filter_matches(&f, &spam, NULL));
	g_assert_true(mcp_filter_matches(&f, &ham, NULL));

	f = parse_filter("{\"unread_only\":true}");
	g_assert_cmpint(f.unread, ==, 1);
	g_assert_cmpint(f.has_attachment, ==, MCP_ANY);
}

static void test_filter_tag(void)
{
	const gchar *tags[] = { "Rechnung", NULL };
	MsgInfo msg;
	McpMsgFilter f;

	init_msg(&msg, 0, 0);
	f = parse_filter("{\"tag\":\"rechnung\"}");
	g_assert_true(mcp_filter_matches(&f, &msg, tags));
	g_assert_false(mcp_filter_matches(&f, &msg, NULL));
	f = parse_filter("{\"tag\":\"Steuer\"}");
	g_assert_false(mcp_filter_matches(&f, &msg, tags));
}

static void test_filter_dates(void)
{
	MsgInfo msg;
	McpMsgFilter f;
	gint64 day = 0;
	McpJson *bad;
	gchar *error = NULL;

	g_assert_true(mcp_parse_day("2026-03-15", &day));
	init_msg(&msg, 0, 0);

	f = parse_filter("{\"since\":\"2026-03-15\",\"until\":\"2026-03-15\"}");
	msg.date_t = (time_t)(day + 12 * 3600);
	g_assert_true(mcp_filter_matches(&f, &msg, NULL));
	msg.date_t = (time_t)(day - 1);
	g_assert_false(mcp_filter_matches(&f, &msg, NULL));
	msg.date_t = (time_t)(day + 24 * 3600);
	g_assert_false(mcp_filter_matches(&f, &msg, NULL));

	g_assert_false(mcp_parse_day("2026-02-30", &day));
	g_assert_false(mcp_parse_day("2026-3-15", &day));
	g_assert_false(mcp_parse_day("15.03.2026", &day));

	bad = mcp_json_parse("{\"since\":\"yesterday\"}", -1, NULL);
	g_assert_false(mcp_filter_parse(bad, &f, &error));
	g_assert_nonnull(error);
	g_free(error);
	mcp_json_free(bad);
}

static MimeInfo *make_part(MimeMediaType type, const gchar *subtype, DispositionType disposition)
{
	MimeInfo *mi = g_new0(MimeInfo, 1);

	mi->type = type;
	mi->subtype = g_strdup(subtype);
	mi->disposition = disposition;
	mi->typeparameters = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	mi->dispositionparameters = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	mi->node = g_node_new(mi);
	return mi;
}

static void free_part(MimeInfo *mi)
{
	g_node_destroy(mi->node);
	g_hash_table_destroy(mi->typeparameters);
	g_hash_table_destroy(mi->dispositionparameters);
	g_free(mi->subtype);
	g_free(mi);
}

static void test_mime_attachment_detection(void)
{
	MimeInfo *body = make_part(MIMETYPE_TEXT, "plain", DISPOSITIONTYPE_UNKNOWN);
	MimeInfo *pdf = make_part(MIMETYPE_APPLICATION, "pdf", DISPOSITIONTYPE_ATTACHMENT);
	MimeInfo *image = make_part(MIMETYPE_IMAGE, "png", DISPOSITIONTYPE_INLINE);
	MimeInfo *sig = make_part(MIMETYPE_APPLICATION, "pgp-signature", DISPOSITIONTYPE_UNKNOWN);
	MimeInfo *named_text = make_part(MIMETYPE_TEXT, "csv", DISPOSITIONTYPE_INLINE);
	MimeInfo *multi = make_part(MIMETYPE_MULTIPART, "mixed", DISPOSITIONTYPE_UNKNOWN);

	g_hash_table_insert(pdf->dispositionparameters, g_strdup("filename"), g_strdup("Rechnung.pdf"));
	g_hash_table_insert(named_text->typeparameters, g_strdup("name"), g_strdup("data.csv"));

	g_assert_false(mcp_part_is_attachment(body));
	g_assert_true(mcp_part_is_attachment(pdf));
	g_assert_true(mcp_part_is_attachment(image));
	g_assert_false(mcp_part_is_attachment(sig));
	g_assert_true(mcp_part_is_attachment(named_text));
	g_assert_false(mcp_part_is_attachment(multi));

	g_assert_cmpstr(mcp_part_filename(pdf), ==, "Rechnung.pdf");
	g_assert_cmpstr(mcp_part_filename(named_text), ==, "data.csv");
	g_assert_null(mcp_part_filename(body));

	g_node_append_data(multi->node, NULL);
	g_assert_false(mcp_part_is_attachment(multi));

	free_part(body);
	free_part(pdf);
	free_part(image);
	free_part(sig);
	free_part(named_text);
	free_part(multi);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/msg-json/spam-set", test_spam_flag_set);
	g_test_add_func("/msg-json/spam-clear", test_spam_flag_clear);
	g_test_add_func("/msg-json/thread-lock", test_thread_and_lock_flags);
	g_test_add_func("/msg-json/color-label", test_color_label);
	g_test_add_func("/msg-json/tags-ids", test_tags_and_ids);
	g_test_add_func("/msg-json/flag-names", test_flag_names);
	g_test_add_func("/msg-json/filter-flags", test_filter_flags);
	g_test_add_func("/msg-json/filter-tag", test_filter_tag);
	g_test_add_func("/msg-json/filter-dates", test_filter_dates);
	g_test_add_func("/msg-json/mime", test_mime_attachment_detection);
	return g_test_run();
}
