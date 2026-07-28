/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Georg Nelles */

#include "claws-features.h"

#include <string.h>
#include <stdarg.h>
#include <glib.h>

#include "common/version.h"
#include "common/utils.h"
#include "folder.h"
#include "procmsg.h"
#include "procheader.h"
#include "procmime.h"
#include "common/file-utils.h"
#include "account.h"
#include "prefs_account.h"
#include "compose.h"
#include "inc.h"
#include "main.h"
#include "mainwindow.h"
#include "folderview.h"

#include "mcp_tools.h"
#include "mcp_plugin.h"

#define MCP_MSGINFO_FREE(mi) procmsg_msginfo_free(&(mi))

#define MCP_MAX_BODY_CHARS 200000

static void fail_with(gchar **error, const gchar *fmt, ...) G_GNUC_PRINTF(2, 3);

static void fail_with(gchar **error, const gchar *fmt, ...)
{
	va_list ap;

	if (error == NULL)
		return;
	va_start(ap, fmt);
	*error = g_strdup_vprintf(fmt, ap);
	va_end(ap);
}

static void jkey(GString *out, const gchar *key)
{
	if (out->len > 0 && out->str[out->len - 1] != '{' && out->str[out->len - 1] != '[')
		g_string_append_c(out, ',');
	mcp_json_escape(out, key);
	g_string_append_c(out, ':');
}

static void jstr(GString *out, const gchar *key, const gchar *value)
{
	jkey(out, key);
	mcp_json_escape(out, value != NULL ? value : "");
}

static void jint(GString *out, const gchar *key, gint64 value)
{
	jkey(out, key);
	g_string_append_printf(out, "%" G_GINT64_FORMAT, value);
}

static void jbool(GString *out, const gchar *key, gboolean value)
{
	jkey(out, key);
	g_string_append(out, value ? "true" : "false");
}

static void jsep(GString *out)
{
	if (out->len > 0 && out->str[out->len - 1] != '[')
		g_string_append_c(out, ',');
}

static gchar *fmt_time(time_t t)
{
	GDateTime *dt;
	gchar *s;

	if (t == 0)
		return g_strdup("");
	dt = g_date_time_new_from_unix_local((gint64)t);
	if (dt == NULL)
		return g_strdup("");
	s = g_date_time_format(dt, "%Y-%m-%d %H:%M");
	g_date_time_unref(dt);
	return s;
}

typedef struct {
	const gchar *needle;
	FolderItem  *found;
} FolderSearch;

static void folder_match_cb(FolderItem *item, gpointer data)
{
	FolderSearch *search = data;
	gchar *id;

	if (search->found != NULL || item == NULL)
		return;
	if (item->name != NULL && g_ascii_strcasecmp(item->name, search->needle) == 0) {
		search->found = item;
		return;
	}
	id = folder_item_get_identifier(item);
	if (id != NULL && g_ascii_strcasecmp(id, search->needle) == 0)
		search->found = item;
	g_free(id);
}

static FolderItem *resolve_folder(const gchar *spec)
{
	FolderItem *item;
	FolderSearch search;

	if (spec == NULL || *spec == '\0')
		return folder_get_default_inbox();

	if (g_ascii_strcasecmp(spec, "inbox") == 0)
		return folder_get_default_inbox();
	if (g_ascii_strcasecmp(spec, "sent") == 0)
		return folder_get_default_outbox();
	if (g_ascii_strcasecmp(spec, "queue") == 0)
		return folder_get_default_queue();
	if (g_ascii_strcasecmp(spec, "drafts") == 0)
		return folder_get_default_draft();
	if (g_ascii_strcasecmp(spec, "trash") == 0)
		return folder_get_default_trash();

	item = folder_find_item_from_identifier(spec);
	if (item != NULL)
		return item;

	search.needle = spec;
	search.found = NULL;
	folder_func_to_all_folders(folder_match_cb, &search);
	return search.found;
}

static FolderItem *require_folder(const McpJson *args, const gchar *key, gchar **error)
{
	const gchar *spec = mcp_json_member_string(args, key, NULL);
	FolderItem *item = resolve_folder(spec);

	if (item == NULL)
		fail_with(error, "folder not found: %s", spec != NULL ? spec : "(default inbox)");
	return item;
}

static MsgInfo *require_msginfo(const McpJson *args, FolderItem *item, gchar **error)
{
	gint msgnum = (gint)mcp_json_member_int(args, "msgnum", -1);
	MsgInfo *msginfo;

	if (msgnum < 0) {
		fail_with(error, "msgnum is required");
		return NULL;
	}
	msginfo = folder_item_get_msginfo(item, msgnum);
	if (msginfo == NULL)
		fail_with(error, "message %d not found in folder", msgnum);
	return msginfo;
}

static gint msginfo_cmp_date_desc(gconstpointer a, gconstpointer b)
{
	const MsgInfo *ma = a;
	const MsgInfo *mb = b;

	if (ma->date_t == mb->date_t) {
		if (ma->msgnum != mb->msgnum)
			return ma->msgnum < mb->msgnum ? 1 : -1;
		return 0;
	}
	return ma->date_t < mb->date_t ? 1 : -1;
}

static void append_msg_summary(GString *out, MsgInfo *msginfo, const gchar *folder_id)
{
	gchar *date = fmt_time(msginfo->date_t);

	g_string_append_c(out, '{');
	jint(out, "msgnum", msginfo->msgnum);
	jstr(out, "folder", folder_id);
	jstr(out, "date", date);
	jstr(out, "from", msginfo->from);
	jstr(out, "to", msginfo->to);
	jstr(out, "subject", msginfo->subject);
	jint(out, "size", (gint64)msginfo->size);
	jbool(out, "unread", MSG_IS_UNREAD(msginfo->flags) ? TRUE : FALSE);
	jbool(out, "new", MSG_IS_NEW(msginfo->flags) ? TRUE : FALSE);
	jbool(out, "marked", MSG_IS_MARKED(msginfo->flags) ? TRUE : FALSE);
	jbool(out, "replied", MSG_IS_REPLIED(msginfo->flags) ? TRUE : FALSE);
	jbool(out, "has_attachment", MSG_IS_WITH_ATTACHMENT(msginfo->flags) ? TRUE : FALSE);
	g_string_append_c(out, '}');
	g_free(date);
}

typedef struct {
	GString  *out;
	gboolean  include_empty;
	guint     count;
} FolderListCtx;

static void folder_list_cb(FolderItem *item, gpointer data)
{
	FolderListCtx *ctx = data;
	gchar *id;

	if (item == NULL || item->path == NULL)
		return;
	if (!ctx->include_empty && item->total_msgs == 0 && item->unread_msgs == 0)
		return;

	id = folder_item_get_identifier(item);
	jsep(ctx->out);
	g_string_append_c(ctx->out, '{');
	jstr(ctx->out, "id", id);
	jstr(ctx->out, "name", item->name);
	jint(ctx->out, "total", item->total_msgs);
	jint(ctx->out, "unread", item->unread_msgs);
	jint(ctx->out, "new", item->new_msgs);
	jbool(ctx->out, "selectable", item->no_select ? FALSE : TRUE);
	g_string_append_c(ctx->out, '}');
	ctx->count++;
	g_free(id);
}

static gchar *tool_list_folders(const McpJson *args, gchar **error)
{
	FolderListCtx ctx;

	(void)error;
	ctx.out = g_string_new("[");
	ctx.include_empty = mcp_json_member_bool(args, "include_empty", FALSE);
	ctx.count = 0;
	folder_func_to_all_folders(folder_list_cb, &ctx);
	g_string_append_c(ctx.out, ']');
	return g_string_free(ctx.out, FALSE);
}

static gchar *tool_list_messages(const McpJson *args, gchar **error)
{
	FolderItem *item = require_folder(args, "folder", error);
	GSList *mlist, *cur;
	GString *out;
	gchar *folder_id;
	gint limit = (gint)mcp_json_member_int(args, "limit", 20);
	gint offset = (gint)mcp_json_member_int(args, "offset", 0);
	gboolean unread_only = mcp_json_member_bool(args, "unread_only", FALSE);
	gint index = 0, emitted = 0;

	if (item == NULL)
		return NULL;
	if (limit <= 0)
		limit = 20;
	if (limit > 500)
		limit = 500;
	if (offset < 0)
		offset = 0;

	mlist = folder_item_get_msg_list(item);
	mlist = g_slist_sort(mlist, msginfo_cmp_date_desc);
	folder_id = folder_item_get_identifier(item);

	out = g_string_new("[");
	for (cur = mlist; cur != NULL; cur = cur->next) {
		MsgInfo *msginfo = cur->data;

		if (unread_only && !MSG_IS_UNREAD(msginfo->flags))
			continue;
		if (index++ < offset)
			continue;
		if (emitted >= limit)
			break;
		jsep(out);
		append_msg_summary(out, msginfo, folder_id);
		emitted++;
	}
	g_string_append_c(out, ']');

	procmsg_msg_list_free(mlist);
	g_free(folder_id);
	return g_string_free(out, FALSE);
}

static gchar *tool_read_message(const McpJson *args, gchar **error)
{
	FolderItem *item = require_folder(args, "folder", error);
	MsgInfo *msginfo;
	GString *out;
	gchar *folder_id, *date, *text;
	gint max_chars = (gint)mcp_json_member_int(args, "max_chars", 20000);

	if (item == NULL)
		return NULL;
	msginfo = require_msginfo(args, item, error);
	if (msginfo == NULL)
		return NULL;

	if (max_chars <= 0)
		max_chars = 20000;
	if (max_chars > MCP_MAX_BODY_CHARS)
		max_chars = MCP_MAX_BODY_CHARS;

	folder_id = folder_item_get_identifier(item);
	date = fmt_time(msginfo->date_t);
	text = NULL;
	{
		FILE *fp = procmime_get_first_text_content(msginfo);
		if (fp != NULL) {
			text = file_read_stream_to_str(fp);
			(void)fclose(fp);
		}
	}

	if (text != NULL && (gint)strlen(text) > max_chars) {
		gchar *cut = g_strndup(text, (gsize)max_chars);
		g_free(text);
		text = g_strconcat(cut, "\n[... truncated ...]", NULL);
		g_free(cut);
	}

	out = g_string_new("{");
	jint(out, "msgnum", msginfo->msgnum);
	jstr(out, "folder", folder_id);
	jstr(out, "date", date);
	jstr(out, "from", msginfo->from);
	jstr(out, "to", msginfo->to);
	jstr(out, "cc", msginfo->cc);
	jstr(out, "subject", msginfo->subject);
	jstr(out, "message_id", msginfo->msgid);
	jbool(out, "unread", MSG_IS_UNREAD(msginfo->flags) ? TRUE : FALSE);
	jbool(out, "has_attachment", MSG_IS_WITH_ATTACHMENT(msginfo->flags) ? TRUE : FALSE);
	jstr(out, "body", text != NULL ? text : "");
	g_string_append_c(out, '}');

	g_free(text);
	g_free(date);
	g_free(folder_id);
	MCP_MSGINFO_FREE(msginfo);
	return g_string_free(out, FALSE);
}

static gboolean text_contains(const gchar *haystack, const gchar *needle_ci)
{
	gchar *folded;
	gboolean hit;

	if (haystack == NULL || *haystack == '\0')
		return FALSE;
	folded = g_utf8_casefold(haystack, -1);
	hit = strstr(folded, needle_ci) != NULL;
	g_free(folded);
	return hit;
}

static gchar *tool_search_messages(const McpJson *args, gchar **error)
{
	FolderItem *item = require_folder(args, "folder", error);
	const gchar *query = mcp_json_member_string(args, "query", NULL);
	gboolean include_body = mcp_json_member_bool(args, "include_body", FALSE);
	gint limit = (gint)mcp_json_member_int(args, "limit", 25);
	gint max_scan = (gint)mcp_json_member_int(args, "max_scan", 2000);
	GSList *mlist, *cur;
	GString *out;
	gchar *folder_id, *needle;
	gint scanned = 0, hits = 0;

	if (item == NULL)
		return NULL;
	if (query == NULL || *query == '\0') {
		fail_with(error, "query is required");
		return NULL;
	}
	if (limit <= 0)
		limit = 25;
	if (limit > 200)
		limit = 200;
	if (max_scan <= 0)
		max_scan = 2000;
	if (max_scan > 20000)
		max_scan = 20000;

	needle = g_utf8_casefold(query, -1);
	mlist = folder_item_get_msg_list(item);
	mlist = g_slist_sort(mlist, msginfo_cmp_date_desc);
	folder_id = folder_item_get_identifier(item);

	out = g_string_new("[");
	for (cur = mlist; cur != NULL && hits < limit && scanned < max_scan; cur = cur->next) {
		MsgInfo *msginfo = cur->data;
		gboolean hit;

		scanned++;
		hit = text_contains(msginfo->subject, needle) ||
		      text_contains(msginfo->from, needle) ||
		      text_contains(msginfo->to, needle) ||
		      text_contains(msginfo->cc, needle);

		if (!hit && include_body) {
			gchar *body = NULL;
			FILE *fp = procmime_get_first_text_content(msginfo);
			if (fp != NULL) {
				body = file_read_stream_to_str(fp);
				(void)fclose(fp);
			}
			hit = text_contains(body, needle);
			g_free(body);
		}
		if (!hit)
			continue;

		jsep(out);
		append_msg_summary(out, msginfo, folder_id);
		hits++;
	}
	g_string_append_c(out, ']');

	procmsg_msg_list_free(mlist);
	g_free(folder_id);
	g_free(needle);
	return g_string_free(out, FALSE);
}

static void add_query_param(GString *uri, const gchar *name, const gchar *value)
{
	gchar *escaped;

	if (value == NULL || *value == '\0')
		return;
	escaped = g_uri_escape_string(value, NULL, FALSE);
	g_string_append_printf(uri, "%c%s=%s",
			       strchr(uri->str, '?') != NULL ? '&' : '?', name, escaped);
	g_free(escaped);
}

static gchar *tool_compose(const McpJson *args, gchar **error)
{
	const gchar *to = mcp_json_member_string(args, "to", "");
	const McpJson *attach = mcp_json_member(args, "attach");
	PrefsAccount *account = NULL;
	GList *files = NULL;
	GString *uri;
	const gchar *from;
	gchar *escaped_to;
	guint i;

	from = mcp_json_member_string(args, "account", NULL);
	if (from != NULL)
		account = account_find_from_address(from, FALSE);

	escaped_to = g_uri_escape_string(to, "@,", FALSE);
	uri = g_string_new(escaped_to);
	g_free(escaped_to);

	add_query_param(uri, "cc", mcp_json_member_string(args, "cc", NULL));
	add_query_param(uri, "bcc", mcp_json_member_string(args, "bcc", NULL));
	add_query_param(uri, "subject", mcp_json_member_string(args, "subject", NULL));
	add_query_param(uri, "body", mcp_json_member_string(args, "body", NULL));

	if (mcp_json_is(attach, MCP_JSON_ARRAY) && mcp_json_length(attach) > 0) {
		for (i = 0; i < mcp_json_length(attach); i++) {
			const gchar *path = mcp_json_get_string(mcp_json_index(attach, i), NULL);
			if (path != NULL && g_file_test(path, G_FILE_TEST_IS_REGULAR))
				files = g_list_append(files, g_strdup(path));
		}
	}

	if (compose_new(account, uri->str, files) == NULL) {
		fail_with(error, "could not open a compose window");
		g_string_free(uri, TRUE);
		g_list_free_full(files, g_free);
		return NULL;
	}

	g_string_free(uri, TRUE);
	g_list_free_full(files, g_free);

	return g_strdup("{\"status\":\"compose window opened, nothing was sent\"}");
}

static gchar *tool_receive_all(const McpJson *args, gchar **error)
{
	MainWindow *mainwin = mainwindow_get_mainwindow();

	(void)args;
	if (mainwin == NULL) {
		fail_with(error, "main window not available");
		return NULL;
	}
	inc_all_account_mail(mainwin, FALSE, FALSE, FALSE);
	return g_strdup("{\"status\":\"receive triggered\"}");
}

static gchar *tool_send_queued(const McpJson *args, gchar **error)
{
	gchar *errstr = NULL;
	gint n;

	(void)args;
	n = procmsg_send_queue(NULL, TRUE, &errstr);
	if (n < 0) {
		fail_with(error, "sending failed: %s", errstr != NULL ? errstr : "unknown error");
		g_free(errstr);
		return NULL;
	}
	g_free(errstr);
	return g_strdup_printf("{\"sent\":%d}", n);
}

static gchar *tool_set_read(const McpJson *args, gchar **error)
{
	FolderItem *item = require_folder(args, "folder", error);
	MsgInfo *msginfo;
	gboolean read = mcp_json_member_bool(args, "read", TRUE);

	if (item == NULL)
		return NULL;
	msginfo = require_msginfo(args, item, error);
	if (msginfo == NULL)
		return NULL;

	if (read)
		procmsg_msginfo_unset_flags(msginfo, MSG_UNREAD | MSG_NEW, 0);
	else
		procmsg_msginfo_set_flags(msginfo, MSG_UNREAD, 0);

	MCP_MSGINFO_FREE(msginfo);
	return g_strdup_printf("{\"msgnum\":%d,\"unread\":%s}",
			       (gint)mcp_json_member_int(args, "msgnum", -1),
			       read ? "false" : "true");
}

static gchar *tool_move_message(const McpJson *args, gchar **error)
{
	FolderItem *item = require_folder(args, "folder", error);
	FolderItem *dest;
	MsgInfo *msginfo;
	gint newnum;

	if (item == NULL)
		return NULL;
	dest = resolve_folder(mcp_json_member_string(args, "target", NULL));
	if (dest == NULL || dest->no_select) {
		fail_with(error, "target folder not found or not selectable");
		return NULL;
	}
	msginfo = require_msginfo(args, item, error);
	if (msginfo == NULL)
		return NULL;

	newnum = folder_item_move_msg(dest, msginfo);
	MCP_MSGINFO_FREE(msginfo);

	if (newnum < 0) {
		fail_with(error, "move failed");
		return NULL;
	}
	return g_strdup_printf("{\"moved_to_msgnum\":%d}", newnum);
}

static gchar *tool_select_folder(const McpJson *args, gchar **error)
{
	MainWindow *mainwin = mainwindow_get_mainwindow();
	FolderItem *item = require_folder(args, "folder", error);
	gchar *id;
	gchar *quoted;
	gchar *result;

	if (item == NULL)
		return NULL;
	if (mainwin == NULL) {
		fail_with(error, "main window not available");
		return NULL;
	}

	folderview_select(mainwin->folderview, item);
	id = folder_item_get_identifier(item);
	quoted = mcp_json_quote(id);
	result = g_strdup_printf("{\"selected\":%s}", quoted);
	g_free(quoted);
	g_free(id);
	return result;
}

static gchar *tool_list_accounts(const McpJson *args, gchar **error)
{
	GList *cur;
	GString *out;

	(void)args;
	(void)error;
	out = g_string_new("[");
	for (cur = account_get_list(); cur != NULL; cur = cur->next) {
		PrefsAccount *ac = cur->data;

		jsep(out);
		g_string_append_c(out, '{');
		jstr(out, "name", ac->account_name);
		jstr(out, "address", ac->address);
		jstr(out, "display_name", ac->name);
		jbool(out, "default", ac->is_default ? TRUE : FALSE);
		jbool(out, "current", ac == cur_account ? TRUE : FALSE);
		g_string_append_c(out, '}');
	}
	g_string_append_c(out, ']');
	return g_string_free(out, FALSE);
}

typedef struct {
	gint total;
	gint unread;
	gint new_msgs;
} StatusCtx;

static void status_cb(FolderItem *item, gpointer data)
{
	StatusCtx *ctx = data;

	if (item == NULL || item->path == NULL)
		return;
	ctx->total += item->total_msgs;
	ctx->unread += item->unread_msgs;
	ctx->new_msgs += item->new_msgs;
}

static gchar *tool_status(const McpJson *args, gchar **error)
{
	StatusCtx ctx = { 0, 0, 0 };
	GString *out;

	(void)args;
	(void)error;
	folder_func_to_all_folders(status_cb, &ctx);

	out = g_string_new("{");
	jint(out, "total", ctx.total);
	jint(out, "unread", ctx.unread);
	jint(out, "new", ctx.new_msgs);
	jstr(out, "plugin_version", MCP_PLUGIN_VERSION);
	jbool(out, "read_only", mcp_plugin_read_only());
	g_string_append_c(out, '}');
	return g_string_free(out, FALSE);
}

#define SCHEMA_EMPTY "{\"type\":\"object\",\"properties\":{}}"

static gchar *id_result(const gchar *key, const gchar *value)
{
	gchar *quoted = mcp_json_quote(value);
	gchar *result = g_strdup_printf("{%s:%s}", key, quoted);

	g_free(quoted);
	return result;
}

static gchar *tool_create_folder(const McpJson *args, gchar **error)
{
	FolderItem *parent = require_folder(args, "parent", error);
	const gchar *name = mcp_json_member_string(args, "name", NULL);
	FolderItem *created;
	gchar *id, *result;

	if (parent == NULL)
		return NULL;
	if (name == NULL || *name == '\0') {
		fail_with(error, "name is required");
		return NULL;
	}
	if (parent->folder == NULL || parent->folder->klass == NULL ||
	    parent->folder->klass->create_folder == NULL) {
		fail_with(error, "this folder type cannot contain subfolders");
		return NULL;
	}

	created = folder_create_folder(parent, name);
	if (created == NULL) {
		fail_with(error, "could not create folder \"%s\"", name);
		return NULL;
	}
	folder_write_list();

	id = folder_item_get_identifier(created);
	result = id_result("\"created\"", id);
	g_free(id);
	return result;
}

static gchar *tool_rename_folder(const McpJson *args, gchar **error)
{
	FolderItem *item = require_folder(args, "folder", error);
	const gchar *name = mcp_json_member_string(args, "name", NULL);
	gchar *id, *result;

	if (item == NULL)
		return NULL;
	if (name == NULL || *name == '\0') {
		fail_with(error, "name is required");
		return NULL;
	}
	if (item->stype != F_NORMAL) {
		fail_with(error, "special folders cannot be renamed");
		return NULL;
	}
	if (folder_item_rename(item, (gchar *)name) < 0) {
		fail_with(error, "rename failed");
		return NULL;
	}
	folder_write_list();

	id = folder_item_get_identifier(item);
	result = id_result("\"renamed\"", id);
	g_free(id);
	return result;
}

static gchar *tool_delete_folder(const McpJson *args, gchar **error)
{
	FolderItem *item = require_folder(args, "folder", error);

	if (item == NULL)
		return NULL;
	if (item->stype != F_NORMAL) {
		fail_with(error, "refusing to delete a special folder");
		return NULL;
	}
	if (item->node != NULL && item->folder != NULL &&
	    item->node == item->folder->node) {
		fail_with(error, "refusing to delete a mailbox root");
		return NULL;
	}
	folder_item_remove(item);
	folder_write_list();
	return g_strdup("{\"deleted\":true}");
}

static gchar *tool_copy_message(const McpJson *args, gchar **error)
{
	FolderItem *item = require_folder(args, "folder", error);
	FolderItem *dest;
	MsgInfo *msginfo;
	gint newnum;

	if (item == NULL)
		return NULL;
	dest = resolve_folder(mcp_json_member_string(args, "target", NULL));
	if (dest == NULL || dest->no_select) {
		fail_with(error, "target folder not found or not selectable");
		return NULL;
	}
	msginfo = require_msginfo(args, item, error);
	if (msginfo == NULL)
		return NULL;

	newnum = folder_item_copy_msg(dest, msginfo);
	MCP_MSGINFO_FREE(msginfo);
	if (newnum < 0) {
		fail_with(error, "copy failed");
		return NULL;
	}
	return g_strdup_printf("{\"copied_to_msgnum\":%d}", newnum);
}

static gchar *tool_delete_message(const McpJson *args, gchar **error)
{
	FolderItem *item = require_folder(args, "folder", error);
	gint msgnum = (gint)mcp_json_member_int(args, "msgnum", -1);
	MsgInfo *msginfo;

	if (item == NULL)
		return NULL;
	msginfo = require_msginfo(args, item, error);
	if (msginfo == NULL)
		return NULL;
	MCP_MSGINFO_FREE(msginfo);

	if (folder_item_remove_msg(item, msgnum) < 0) {
		fail_with(error, "delete failed");
		return NULL;
	}
	return g_strdup_printf("{\"deleted_msgnum\":%d}", msgnum);
}

static gchar *tool_empty_trash(const McpJson *args, gchar **error)
{
	FolderItem *trash = resolve_folder(mcp_json_member_string(args, "folder", "trash"));

	if (trash == NULL) {
		fail_with(error, "no trash folder");
		return NULL;
	}
	if (folder_item_remove_all_msg(trash) < 0) {
		fail_with(error, "emptying trash failed");
		return NULL;
	}
	return g_strdup("{\"emptied\":true}");
}

static gchar *tool_mark_all_read(const McpJson *args, gchar **error)
{
	FolderItem *item = require_folder(args, "folder", error);
	GSList *mlist, *cur;
	gint n = 0;

	if (item == NULL)
		return NULL;

	mlist = folder_item_get_msg_list(item);
	for (cur = mlist; cur != NULL; cur = cur->next) {
		MsgInfo *msginfo = cur->data;

		if (MSG_IS_UNREAD(msginfo->flags) || MSG_IS_NEW(msginfo->flags)) {
			procmsg_msginfo_unset_flags(msginfo, MSG_UNREAD | MSG_NEW, 0);
			n++;
		}
	}
	procmsg_msg_list_free(mlist);
	return g_strdup_printf("{\"marked_read\":%d}", n);
}

static gchar *tool_set_flag(const McpJson *args, gchar **error)
{
	FolderItem *item = require_folder(args, "folder", error);
	const gchar *flag = mcp_json_member_string(args, "flag", NULL);
	gboolean value = mcp_json_member_bool(args, "value", TRUE);
	MsgInfo *msginfo;
	MsgPermFlags bit;
	gchar *quoted, *result;

	if (item == NULL)
		return NULL;
	if (flag == NULL) {
		fail_with(error, "flag is required");
		return NULL;
	}
	if (g_ascii_strcasecmp(flag, "marked") == 0)
		bit = MSG_MARKED;
	else if (g_ascii_strcasecmp(flag, "locked") == 0)
		bit = MSG_LOCKED;
	else if (g_ascii_strcasecmp(flag, "unread") == 0)
		bit = MSG_UNREAD;
	else if (g_ascii_strcasecmp(flag, "replied") == 0)
		bit = MSG_REPLIED;
	else if (g_ascii_strcasecmp(flag, "forwarded") == 0)
		bit = MSG_FORWARDED;
	else if (g_ascii_strcasecmp(flag, "ignore_thread") == 0)
		bit = MSG_IGNORE_THREAD;
	else if (g_ascii_strcasecmp(flag, "watch_thread") == 0)
		bit = MSG_WATCH_THREAD;
	else {
		fail_with(error, "unknown flag: %s", flag);
		return NULL;
	}

	msginfo = require_msginfo(args, item, error);
	if (msginfo == NULL)
		return NULL;

	if (value)
		procmsg_msginfo_set_flags(msginfo, bit, 0);
	else
		procmsg_msginfo_unset_flags(msginfo, bit, 0);
	MCP_MSGINFO_FREE(msginfo);

	quoted = mcp_json_quote(flag);
	result = g_strdup_printf("{\"flag\":%s,\"value\":%s}",
				 quoted, value ? "true" : "false");
	g_free(quoted);
	return result;
}

static gchar *tool_get_source(const McpJson *args, gchar **error)
{
	FolderItem *item = require_folder(args, "folder", error);
	MsgInfo *msginfo;
	gchar *file, *contents = NULL;
	gsize len = 0;
	gint max_chars = (gint)mcp_json_member_int(args, "max_chars", 50000);
	GString *out;

	if (item == NULL)
		return NULL;
	msginfo = require_msginfo(args, item, error);
	if (msginfo == NULL)
		return NULL;

	file = procmsg_get_message_file(msginfo);
	MCP_MSGINFO_FREE(msginfo);
	if (file == NULL || !g_file_get_contents(file, &contents, &len, NULL)) {
		fail_with(error, "could not read message file");
		g_free(file);
		return NULL;
	}
	g_free(file);

	if (max_chars <= 0)
		max_chars = 50000;
	if (max_chars > MCP_MAX_BODY_CHARS)
		max_chars = MCP_MAX_BODY_CHARS;
	if ((gint)len > max_chars)
		contents[max_chars] = '\0';

	out = g_string_new("{");
	jstr(out, "source", contents);
	jbool(out, "truncated", (gint)len > max_chars);
	g_string_append_c(out, '}');
	g_free(contents);
	return g_string_free(out, FALSE);
}

static gchar *tool_export_message(const McpJson *args, gchar **error)
{
	FolderItem *item = require_folder(args, "folder", error);
	const gchar *dest = mcp_json_member_string(args, "path", NULL);
	MsgInfo *msginfo;
	gchar *file, *contents = NULL;
	gsize len = 0;

	if (item == NULL)
		return NULL;
	if (dest == NULL || *dest == '\0') {
		fail_with(error, "path is required");
		return NULL;
	}
	msginfo = require_msginfo(args, item, error);
	if (msginfo == NULL)
		return NULL;

	file = procmsg_get_message_file(msginfo);
	MCP_MSGINFO_FREE(msginfo);
	if (file == NULL || !g_file_get_contents(file, &contents, &len, NULL)) {
		fail_with(error, "could not read message");
		g_free(file);
		return NULL;
	}
	g_free(file);

	if (!g_file_set_contents(dest, contents, (gssize)len, NULL)) {
		fail_with(error, "could not write to %s", dest);
		g_free(contents);
		return NULL;
	}
	g_free(contents);
	return g_strdup_printf("{\"exported_bytes\":%" G_GSIZE_FORMAT "}", len);
}

static gchar *tool_import_message(const McpJson *args, gchar **error)
{
	FolderItem *item = require_folder(args, "folder", error);
	const gchar *path = mcp_json_member_string(args, "path", NULL);
	MsgFlags flags;
	gint newnum;

	if (item == NULL)
		return NULL;
	if (item->no_select) {
		fail_with(error, "target folder is not selectable");
		return NULL;
	}
	if (path == NULL || !g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
		fail_with(error, "file not found: %s", path != NULL ? path : "(none)");
		return NULL;
	}

	flags.perm_flags = MSG_NEW | MSG_UNREAD;
	flags.tmp_flags = 0;
	newnum = folder_item_add_msg(item, path, &flags, FALSE);
	if (newnum < 0) {
		fail_with(error, "import failed");
		return NULL;
	}
	return g_strdup_printf("{\"msgnum\":%d}", newnum);
}

static gchar *tool_reply(const McpJson *args, gchar **error)
{
	MainWindow *mainwin = mainwindow_get_mainwindow();
	FolderItem *item = require_folder(args, "folder", error);
	MsgInfo *msginfo;
	gboolean to_all = mcp_json_member_bool(args, "to_all", FALSE);
	GSList *msginfo_list;
	guint action;

	if (item == NULL)
		return NULL;
	if (mainwin == NULL) {
		fail_with(error, "main window not available");
		return NULL;
	}
	msginfo = require_msginfo(args, item, error);
	if (msginfo == NULL)
		return NULL;

	action = to_all ? COMPOSE_REPLY_TO_ALL_WITH_QUOTE : COMPOSE_REPLY_WITH_QUOTE;
	msginfo_list = g_slist_append(NULL, msginfo);
	compose_reply_from_messageview(mainwin->messageview, msginfo_list, action);
	g_slist_free(msginfo_list);
	MCP_MSGINFO_FREE(msginfo);

	return g_strdup("{\"status\":\"reply window opened, nothing was sent\"}");
}

static gchar *tool_forward(const McpJson *args, gchar **error)
{
	FolderItem *item = require_folder(args, "folder", error);
	const gchar *acc = mcp_json_member_string(args, "account", NULL);
	gboolean as_attach = mcp_json_member_bool(args, "as_attachment", FALSE);
	PrefsAccount *account = NULL;
	MsgInfo *msginfo;
	Compose *compose;

	if (item == NULL)
		return NULL;
	if (acc != NULL)
		account = account_find_from_address(acc, FALSE);
	msginfo = require_msginfo(args, item, error);
	if (msginfo == NULL)
		return NULL;

	compose = compose_forward(account, msginfo, as_attach, NULL, FALSE, FALSE);
	MCP_MSGINFO_FREE(msginfo);
	if (compose == NULL) {
		fail_with(error, "could not open a forward window");
		return NULL;
	}
	return g_strdup("{\"status\":\"forward window opened, nothing was sent\"}");
}

static const McpTool tools[] = {
	{
		"list_folders",
		"List all Claws Mail folders with message, unread and new counts.",
		"{\"type\":\"object\",\"properties\":{"
		"\"include_empty\":{\"type\":\"boolean\",\"description\":\"also list folders without messages\"}"
		"}}",
		tool_list_folders, FALSE
	},
	{
		"list_messages",
		"List messages of a folder, newest first.",
		"{\"type\":\"object\",\"properties\":{"
		"\"folder\":{\"type\":\"string\",\"description\":\"folder identifier (#mh/Mail/inbox) or name; defaults to inbox\"},"
		"\"limit\":{\"type\":\"integer\",\"description\":\"max messages (default 20, max 500)\"},"
		"\"offset\":{\"type\":\"integer\"},"
		"\"unread_only\":{\"type\":\"boolean\"}"
		"}}",
		tool_list_messages, FALSE
	},
	{
		"read_message",
		"Read a single message: headers plus its first text part.",
		"{\"type\":\"object\",\"properties\":{"
		"\"folder\":{\"type\":\"string\"},"
		"\"msgnum\":{\"type\":\"integer\",\"description\":\"message number as returned by list_messages\"},"
		"\"max_chars\":{\"type\":\"integer\",\"description\":\"truncate body (default 20000)\"}"
		"},\"required\":[\"msgnum\"]}",
		tool_read_message, FALSE
	},
	{
		"search_messages",
		"Case-insensitive substring search over a folder (headers, optionally body).",
		"{\"type\":\"object\",\"properties\":{"
		"\"query\":{\"type\":\"string\"},"
		"\"folder\":{\"type\":\"string\"},"
		"\"limit\":{\"type\":\"integer\"},"
		"\"include_body\":{\"type\":\"boolean\",\"description\":\"also scan the text body (slower)\"},"
		"\"max_scan\":{\"type\":\"integer\"}"
		"},\"required\":[\"query\"]}",
		tool_search_messages, FALSE
	},
	{
		"compose",
		"Open a compose window prefilled with recipient, subject, body and attachments. Never sends by itself.",
		"{\"type\":\"object\",\"properties\":{"
		"\"to\":{\"type\":\"string\"},"
		"\"cc\":{\"type\":\"string\"},"
		"\"bcc\":{\"type\":\"string\"},"
		"\"subject\":{\"type\":\"string\"},"
		"\"body\":{\"type\":\"string\"},"
		"\"account\":{\"type\":\"string\",\"description\":\"sender address of the account to use\"},"
		"\"attach\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"absolute file paths\"}"
		"},\"required\":[\"to\"]}",
		tool_compose, TRUE
	},
	{
		"receive_all",
		"Fetch new mail for all accounts.",
		SCHEMA_EMPTY,
		tool_receive_all, TRUE
	},
	{
		"send_queued",
		"Send every message sitting in the queue folders.",
		SCHEMA_EMPTY,
		tool_send_queued, TRUE
	},
	{
		"set_read",
		"Mark a message as read or unread.",
		"{\"type\":\"object\",\"properties\":{"
		"\"folder\":{\"type\":\"string\"},"
		"\"msgnum\":{\"type\":\"integer\"},"
		"\"read\":{\"type\":\"boolean\",\"description\":\"true marks read (default), false marks unread\"}"
		"},\"required\":[\"msgnum\"]}",
		tool_set_read, TRUE
	},
	{
		"move_message",
		"Move a message to another folder (use target \"trash\" instead of deleting).",
		"{\"type\":\"object\",\"properties\":{"
		"\"folder\":{\"type\":\"string\"},"
		"\"msgnum\":{\"type\":\"integer\"},"
		"\"target\":{\"type\":\"string\"}"
		"},\"required\":[\"msgnum\",\"target\"]}",
		tool_move_message, TRUE
	},
	{
		"select_folder",
		"Show a folder in the Claws Mail window.",
		"{\"type\":\"object\",\"properties\":{"
		"\"folder\":{\"type\":\"string\"}"
		"}}",
		tool_select_folder, FALSE
	},
	{
		"list_accounts",
		"List the configured mail accounts.",
		SCHEMA_EMPTY,
		tool_list_accounts, FALSE
	},
	{
		"status",
		"Overall mailbox counters and plugin state.",
		SCHEMA_EMPTY,
		tool_status, FALSE
	},
	{
		"create_folder",
		"Create a new subfolder under an existing folder.",
		"{\"type\":\"object\",\"properties\":{"
		"\"parent\":{\"type\":\"string\",\"description\":\"parent folder identifier or name\"},"
		"\"name\":{\"type\":\"string\",\"description\":\"name of the new folder\"}"
		"},\"required\":[\"parent\",\"name\"]}",
		tool_create_folder, TRUE
	},
	{
		"rename_folder",
		"Rename an existing user folder (special folders are refused).",
		"{\"type\":\"object\",\"properties\":{"
		"\"folder\":{\"type\":\"string\"},"
		"\"name\":{\"type\":\"string\"}"
		"},\"required\":[\"folder\",\"name\"]}",
		tool_rename_folder, TRUE
	},
	{
		"delete_folder",
		"Delete a user folder and its messages (special folders and roots are refused).",
		"{\"type\":\"object\",\"properties\":{"
		"\"folder\":{\"type\":\"string\"}"
		"},\"required\":[\"folder\"]}",
		tool_delete_folder, TRUE
	},
	{
		"copy_message",
		"Copy a message into another folder, keeping the original.",
		"{\"type\":\"object\",\"properties\":{"
		"\"folder\":{\"type\":\"string\"},"
		"\"msgnum\":{\"type\":\"integer\"},"
		"\"target\":{\"type\":\"string\"}"
		"},\"required\":[\"msgnum\",\"target\"]}",
		tool_copy_message, TRUE
	},
	{
		"delete_message",
		"Permanently remove a message from its folder. Prefer move_message to trash instead.",
		"{\"type\":\"object\",\"properties\":{"
		"\"folder\":{\"type\":\"string\"},"
		"\"msgnum\":{\"type\":\"integer\"}"
		"},\"required\":[\"msgnum\"]}",
		tool_delete_message, TRUE
	},
	{
		"empty_trash",
		"Permanently remove every message from the trash folder.",
		"{\"type\":\"object\",\"properties\":{"
		"\"folder\":{\"type\":\"string\",\"description\":\"trash folder, defaults to the default trash\"}"
		"}}",
		tool_empty_trash, TRUE
	},
	{
		"mark_all_read",
		"Mark every unread message in a folder as read.",
		"{\"type\":\"object\",\"properties\":{"
		"\"folder\":{\"type\":\"string\"}"
		"}}",
		tool_mark_all_read, TRUE
	},
	{
		"set_flag",
		"Set or clear a message flag.",
		"{\"type\":\"object\",\"properties\":{"
		"\"folder\":{\"type\":\"string\"},"
		"\"msgnum\":{\"type\":\"integer\"},"
		"\"flag\":{\"type\":\"string\",\"enum\":[\"marked\",\"locked\",\"unread\","
		"\"replied\",\"forwarded\",\"ignore_thread\",\"watch_thread\"]},"
		"\"value\":{\"type\":\"boolean\",\"description\":\"true sets the flag (default), false clears it\"}"
		"},\"required\":[\"msgnum\",\"flag\"]}",
		tool_set_flag, TRUE
	},
	{
		"get_source",
		"Return the raw RFC822 source of a message (headers and body).",
		"{\"type\":\"object\",\"properties\":{"
		"\"folder\":{\"type\":\"string\"},"
		"\"msgnum\":{\"type\":\"integer\"},"
		"\"max_chars\":{\"type\":\"integer\",\"description\":\"truncate (default 50000)\"}"
		"},\"required\":[\"msgnum\"]}",
		tool_get_source, FALSE
	},
	{
		"export_message",
		"Write a message's raw source to a file on disk.",
		"{\"type\":\"object\",\"properties\":{"
		"\"folder\":{\"type\":\"string\"},"
		"\"msgnum\":{\"type\":\"integer\"},"
		"\"path\":{\"type\":\"string\",\"description\":\"absolute destination path\"}"
		"},\"required\":[\"msgnum\",\"path\"]}",
		tool_export_message, TRUE
	},
	{
		"import_message",
		"Add an RFC822 (.eml) file from disk into a folder as a new message.",
		"{\"type\":\"object\",\"properties\":{"
		"\"folder\":{\"type\":\"string\"},"
		"\"path\":{\"type\":\"string\",\"description\":\"absolute path to the .eml file\"}"
		"},\"required\":[\"folder\",\"path\"]}",
		tool_import_message, TRUE
	},
	{
		"reply",
		"Open a reply compose window for a message. Never sends by itself.",
		"{\"type\":\"object\",\"properties\":{"
		"\"folder\":{\"type\":\"string\"},"
		"\"msgnum\":{\"type\":\"integer\"},"
		"\"to_all\":{\"type\":\"boolean\",\"description\":\"reply to all recipients\"}"
		"},\"required\":[\"msgnum\"]}",
		tool_reply, TRUE
	},
	{
		"forward",
		"Open a forward compose window for a message. Never sends by itself.",
		"{\"type\":\"object\",\"properties\":{"
		"\"folder\":{\"type\":\"string\"},"
		"\"msgnum\":{\"type\":\"integer\"},"
		"\"account\":{\"type\":\"string\",\"description\":\"sender address of the account to use\"},"
		"\"as_attachment\":{\"type\":\"boolean\",\"description\":\"forward as attachment instead of inline\"}"
		"},\"required\":[\"msgnum\"]}",
		tool_forward, TRUE
	}
};

const McpTool *mcp_tools_all(guint *n_tools)
{
	if (n_tools != NULL)
		*n_tools = G_N_ELEMENTS(tools);
	return tools;
}

const McpTool *mcp_tools_find(const gchar *name)
{
	guint i;

	if (name == NULL)
		return NULL;
	for (i = 0; i < G_N_ELEMENTS(tools); i++) {
		if (strcmp(tools[i].name, name) == 0)
			return &tools[i];
	}
	return NULL;
}
