/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Georg Nelles */

#include "claws-features.h"

#include <string.h>
#include <stdarg.h>
#include <glib.h>
#include <glib/gstdio.h>

#include "common/version.h"
#include "common/utils.h"
#include "common/tags.h"
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
#include "mcp_msg_json.h"
#include "mcp_paths.h"

#define MCP_MSGINFO_FREE(mi) procmsg_msginfo_free(&(mi))

#define MCP_MAX_BODY_CHARS 200000
#define MCP_MAX_BULK 1000
#define MCP_DEFAULT_INLINE_BYTES (1024 * 1024)
#define MCP_MAX_INLINE_BYTES (8 * 1024 * 1024)

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

static gchar **msg_tags(const MsgInfo *msginfo)
{
	GPtrArray *names = g_ptr_array_new();
	GSList *cur;

	for (cur = msginfo->tags; cur != NULL; cur = cur->next) {
		const gchar *name = tags_get_tag(GPOINTER_TO_INT(cur->data));

		if (name != NULL)
			g_ptr_array_add(names, g_strdup(name));
	}
	g_ptr_array_add(names, NULL);
	return (gchar **)g_ptr_array_free(names, FALSE);
}

static gboolean mime_has_attachment(MimeInfo *root)
{
	MimeInfo *cur;

	for (cur = root; cur != NULL; cur = procmime_mimeinfo_next(cur)) {
		if (mcp_part_is_attachment(cur))
			return TRUE;
	}
	return FALSE;
}

static gboolean msg_has_attachment(MsgInfo *msginfo)
{
	MimeInfo *root;
	gboolean found;

	if (MSG_IS_WITH_ATTACHMENT(msginfo->flags))
		return TRUE;
	if (MSG_IS_SCANNED(msginfo->flags) || !MSG_IS_MULTIPART(msginfo->flags))
		return FALSE;
	if ((MSG_IS_IMAP(msginfo->flags) || MSG_IS_NEWS(msginfo->flags)) &&
	    !MSG_IS_FULLY_CACHED(msginfo->flags))
		return FALSE;
	root = procmime_scan_message(msginfo);
	if (root == NULL)
		return FALSE;
	found = mime_has_attachment(root);
	procmime_mimeinfo_free_all(&root);
	return found;
}

static void append_msg(GString *out, MsgInfo *msginfo, const gchar *folder_id, gchar **tags)
{
	jsep(out);
	append_msg_summary(out, msginfo, folder_id, (const gchar *const *)tags,
			   msg_has_attachment(msginfo));
}

static gchar *file_root(gchar **error)
{
	const gchar *root = mcp_plugin_file_dir();

	if (g_mkdir_with_parents(root, 0700) != 0) {
		fail_with(error, "cannot create file_dir %s", root);
		return NULL;
	}
	return g_strdup(root);
}

static gchar *resolve_user_path(const gchar *requested, gchar **error)
{
	gchar *root = file_root(error);
	gchar *path;

	if (root == NULL)
		return NULL;
	path = mcp_path_resolve(root, requested);
	if (path == NULL)
		fail_with(error, "path must be inside file_dir (%s): %s", root,
			  requested != NULL ? requested : "(none)");
	g_free(root);
	return path;
}

static gchar *json_body_cut(gchar *text, gint max_chars, gboolean *truncated)
{
	gsize keep;

	*truncated = FALSE;
	if (text == NULL)
		return NULL;
	keep = mcp_utf8_boundary(text, (gsize)max_chars);
	if (text[keep] != '\0') {
		text[keep] = '\0';
		*truncated = TRUE;
	}
	return text;
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
	McpMsgFilter filter;
	gint limit = (gint)mcp_json_member_int(args, "limit", 20);
	gint offset = (gint)mcp_json_member_int(args, "offset", 0);
	gint matched = 0, emitted = 0;
	gboolean has_more = FALSE, exact = TRUE;

	if (item == NULL)
		return NULL;
	if (!mcp_filter_parse(args, &filter, error))
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

	out = g_string_new("{\"messages\":[");
	for (cur = mlist; cur != NULL; cur = cur->next) {
		MsgInfo *msginfo = cur->data;
		gchar **tags = msg_tags(msginfo);
		gboolean keep = mcp_filter_matches(&filter, msginfo, (const gchar *const *)tags);

		if (keep && filter.has_attachment != MCP_ANY)
			keep = mcp_tri_ok(filter.has_attachment, msg_has_attachment(msginfo));
		if (!keep) {
			g_strfreev(tags);
			continue;
		}
		matched++;
		if (matched > offset && emitted < limit) {
			append_msg(out, msginfo, folder_id, tags);
			emitted++;
		} else if (matched > offset) {
			has_more = TRUE;
			if (filter.has_attachment != MCP_ANY) {
				exact = FALSE;
				g_strfreev(tags);
				break;
			}
		}
		g_strfreev(tags);
	}
	g_string_append_c(out, ']');
	if (exact)
		jint(out, "total", matched);
	jint(out, "offset", offset);
	jint(out, "returned", emitted);
	jbool(out, "has_more", has_more);
	g_string_append_c(out, '}');

	procmsg_msg_list_free(mlist);
	g_free(folder_id);
	return g_string_free(out, FALSE);
}

static void append_attachments(GString *out, MimeInfo *root, gint *count)
{
	MimeInfo *cur;
	gint index = 0;

	jkey(out, "attachments");
	g_string_append_c(out, '[');
	for (cur = root; cur != NULL; cur = procmime_mimeinfo_next(cur), index++) {
		gchar *type;

		if (!mcp_part_is_attachment(cur))
			continue;
		type = procmime_get_content_type_str(cur->type, cur->subtype);
		jsep(out);
		g_string_append_c(out, '{');
		jint(out, "part", index);
		jstr(out, "filename", mcp_part_filename(cur));
		jstr(out, "content_type", type);
		jint(out, "encoded_size", cur->length);
		jbool(out, "inline", cur->disposition == DISPOSITIONTYPE_INLINE);
		g_string_append_c(out, '}');
		g_free(type);
		(*count)++;
	}
	g_string_append_c(out, ']');
}

static gchar *tool_read_message(const McpJson *args, gchar **error)
{
	FolderItem *item = require_folder(args, "folder", error);
	MsgInfo *msginfo;
	MimeInfo *root;
	GString *out, *attachments;
	gchar *folder_id, *text = NULL;
	gchar **tags;
	gint count = 0;
	gboolean truncated;
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

	{
		FILE *fp = procmime_get_first_text_content(msginfo);
		if (fp != NULL) {
			text = file_read_stream_to_str(fp);
			(void)fclose(fp);
		}
	}
	text = json_body_cut(text, max_chars, &truncated);

	attachments = g_string_new("{");
	root = procmime_scan_message(msginfo);
	append_attachments(attachments, root, &count);
	procmime_mimeinfo_free_all(&root);

	folder_id = folder_item_get_identifier(item);
	tags = msg_tags(msginfo);

	out = g_string_new("{");
	append_msg_fields(out, msginfo, folder_id, (const gchar *const *)tags, count > 0);
	jstr(out, "cc", msginfo->cc);
	g_string_append_c(out, ',');
	g_string_append(out, attachments->str + 1);
	jstr(out, "body", text != NULL ? text : "");
	jbool(out, "body_truncated", truncated);
	g_string_append_c(out, '}');

	g_string_free(attachments, TRUE);
	g_strfreev(tags);
	g_free(text);
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
	McpMsgFilter filter;
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
	if (!mcp_filter_parse(args, &filter, error))
		return NULL;
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
		gchar **tags = msg_tags(msginfo);
		gboolean hit;

		scanned++;
		if (!mcp_filter_matches(&filter, msginfo, (const gchar *const *)tags)) {
			g_strfreev(tags);
			continue;
		}
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
		if (hit && filter.has_attachment != MCP_ANY)
			hit = mcp_tri_ok(filter.has_attachment, msg_has_attachment(msginfo));
		if (hit) {
			append_msg(out, msginfo, folder_id, tags);
			hits++;
		}
		g_strfreev(tags);
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

	if (mcp_json_is(attach, MCP_JSON_ARRAY)) {
		for (i = 0; i < mcp_json_length(attach); i++) {
			const gchar *requested = mcp_json_get_string(mcp_json_index(attach, i), NULL);
			gchar *path = resolve_user_path(requested, error);

			if (path == NULL) {
				g_list_free_full(files, g_free);
				return NULL;
			}
			if (!g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
				fail_with(error, "attachment not found: %s", path);
				g_free(path);
				g_list_free_full(files, g_free);
				return NULL;
			}
			files = g_list_append(files, path);
		}
	}

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
	jstr(out, "file_dir", mcp_plugin_file_dir());
	jbool(out, "spam_learner", procmsg_spam_can_learn());
	g_string_append_c(out, '}');
	return g_string_free(out, FALSE);
}

#define SCHEMA_EMPTY "{\"type\":\"object\",\"properties\":{}}"

#define FILTER_PROPS \
	"\"unread\":{\"type\":\"boolean\",\"description\":\"only unread (true) or only read (false)\"}," \
	"\"spam\":{\"type\":\"boolean\",\"description\":\"only messages with (true) or without (false) the Claws spam flag\"}," \
	"\"marked\":{\"type\":\"boolean\"}," \
	"\"has_attachment\":{\"type\":\"boolean\"}," \
	"\"tag\":{\"type\":\"string\",\"description\":\"only messages carrying this Claws tag\"}," \
	"\"since\":{\"type\":\"string\",\"description\":\"YYYY-MM-DD, inclusive\"}," \
	"\"until\":{\"type\":\"string\",\"description\":\"YYYY-MM-DD, inclusive\"}"

#define MSGNUMS_PROP \
	"\"msgnums\":{\"type\":\"array\",\"items\":{\"type\":\"integer\"},\"description\":\"message numbers, at most 1000\"}"

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

static void apply_flag(MsgInfo *msginfo, MsgPermFlags bit, gboolean value)
{
	if (value)
		procmsg_msginfo_set_flags(msginfo, bit, 0);
	else if (bit == MSG_UNREAD)
		procmsg_msginfo_unset_flags(msginfo, MSG_UNREAD | MSG_NEW, 0);
	else
		procmsg_msginfo_unset_flags(msginfo, bit, 0);
}

static gboolean require_flag(const McpJson *args, MsgPermFlags *bit, gchar **error)
{
	const gchar *flag = mcp_json_member_string(args, "flag", NULL);
	guint found;

	if (flag == NULL) {
		fail_with(error, "flag is required");
		return FALSE;
	}
	if (!mcp_flag_from_name(flag, &found)) {
		fail_with(error, "unknown flag: %s", flag);
		return FALSE;
	}
	*bit = (MsgPermFlags)found;
	return TRUE;
}

static GSList *require_msginfos(const McpJson *args, FolderItem *item, GString *missing,
				gchar **error)
{
	const McpJson *nums = mcp_json_member(args, "msgnums");
	GHashTable *seen;
	GSList *list = NULL;
	guint i, n;

	n = mcp_json_is(nums, MCP_JSON_ARRAY) ? mcp_json_length(nums) : 0;
	if (n == 0) {
		fail_with(error, "msgnums must be a non-empty array of message numbers");
		return NULL;
	}
	if (n > MCP_MAX_BULK) {
		fail_with(error, "at most %d messages per call", MCP_MAX_BULK);
		return NULL;
	}

	seen = g_hash_table_new(g_direct_hash, g_direct_equal);
	g_string_append_c(missing, '[');
	for (i = 0; i < n; i++) {
		gint num = (gint)mcp_json_get_int(mcp_json_index(nums, i), -1);
		MsgInfo *msginfo;

		if (num < 0 || g_hash_table_contains(seen, GINT_TO_POINTER(num)))
			continue;
		g_hash_table_add(seen, GINT_TO_POINTER(num));
		msginfo = folder_item_get_msginfo(item, num);
		if (msginfo != NULL) {
			list = g_slist_prepend(list, msginfo);
		} else {
			jsep(missing);
			g_string_append_printf(missing, "%d", num);
		}
	}
	g_string_append_c(missing, ']');
	g_hash_table_destroy(seen);

	if (list == NULL)
		fail_with(error, "none of the given messages exist in this folder");
	return g_slist_reverse(list);
}

static gchar *bulk_result(const gchar *key, guint count, GString *missing)
{
	gchar *result = g_strdup_printf("{\"%s\":%u,\"missing\":%s}", key, count, missing->str);

	g_string_free(missing, TRUE);
	return result;
}

static gchar *tool_set_flag(const McpJson *args, gchar **error)
{
	FolderItem *item = require_folder(args, "folder", error);
	gboolean value = mcp_json_member_bool(args, "value", TRUE);
	MsgInfo *msginfo;
	MsgPermFlags bit;
	gchar *quoted, *result;

	if (item == NULL || !require_flag(args, &bit, error))
		return NULL;
	msginfo = require_msginfo(args, item, error);
	if (msginfo == NULL)
		return NULL;

	apply_flag(msginfo, bit, value);
	MCP_MSGINFO_FREE(msginfo);

	quoted = mcp_json_quote(mcp_json_member_string(args, "flag", ""));
	result = g_strdup_printf("{\"flag\":%s,\"value\":%s}",
				 quoted, value ? "true" : "false");
	g_free(quoted);
	return result;
}

static gchar *tool_set_flags(const McpJson *args, gchar **error)
{
	FolderItem *item = require_folder(args, "folder", error);
	gboolean value = mcp_json_member_bool(args, "value", TRUE);
	GString *missing;
	GSList *list, *cur;
	MsgPermFlags bit;
	guint count = 0;

	if (item == NULL || !require_flag(args, &bit, error))
		return NULL;
	missing = g_string_new(NULL);
	list = require_msginfos(args, item, missing, error);
	if (list == NULL) {
		g_string_free(missing, TRUE);
		return NULL;
	}
	for (cur = list; cur != NULL; cur = cur->next, count++)
		apply_flag(cur->data, bit, value);
	procmsg_msg_list_free(list);
	return bulk_result("updated", count, missing);
}

static gchar *tool_move_messages(const McpJson *args, gchar **error)
{
	FolderItem *item = require_folder(args, "folder", error);
	FolderItem *dest;
	GString *missing;
	GSList *list;
	guint count;

	if (item == NULL)
		return NULL;
	dest = resolve_folder(mcp_json_member_string(args, "target", NULL));
	if (dest == NULL || dest->no_select) {
		fail_with(error, "target folder not found or not selectable");
		return NULL;
	}
	if (dest == item) {
		fail_with(error, "target is the source folder");
		return NULL;
	}
	missing = g_string_new(NULL);
	list = require_msginfos(args, item, missing, error);
	if (list == NULL) {
		g_string_free(missing, TRUE);
		return NULL;
	}
	count = g_slist_length(list);
	if (folder_item_move_msgs(dest, list) < 0) {
		procmsg_msg_list_free(list);
		g_string_free(missing, TRUE);
		fail_with(error, "move failed");
		return NULL;
	}
	procmsg_msg_list_free(list);
	return bulk_result("moved", count, missing);
}

static gchar *tool_learn_spam(const McpJson *args, gchar **error)
{
	FolderItem *item = require_folder(args, "folder", error);
	gboolean spam = mcp_json_member_bool(args, "spam", TRUE);
	GString *missing;
	GSList *list, *cur;
	guint count = 0;

	if (item == NULL)
		return NULL;
	if (!procmsg_spam_can_learn()) {
		fail_with(error, "no spam learner is active; enable the Bogofilter or SpamAssassin plugin");
		return NULL;
	}
	missing = g_string_new(NULL);
	list = require_msginfos(args, item, missing, error);
	if (list == NULL) {
		g_string_free(missing, TRUE);
		return NULL;
	}
	if (procmsg_spam_learner_learn(NULL, list, spam) != 0) {
		procmsg_msg_list_free(list);
		g_string_free(missing, TRUE);
		fail_with(error, "the spam learner reported an error");
		return NULL;
	}
	for (cur = list; cur != NULL; cur = cur->next, count++)
		apply_flag(cur->data, MSG_SPAM, spam);
	procmsg_msg_list_free(list);
	return bulk_result(spam ? "learned_spam" : "learned_ham", count, missing);
}

static gchar *tool_get_source(const McpJson *args, gchar **error)
{
	FolderItem *item = require_folder(args, "folder", error);
	MsgInfo *msginfo;
	gchar *file, *contents = NULL;
	gsize len = 0;
	gboolean truncated;
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
	json_body_cut(contents, max_chars, &truncated);

	out = g_string_new("{");
	jstr(out, "source", contents);
	jbool(out, "truncated", truncated);
	g_string_append_c(out, '}');
	g_free(contents);
	return g_string_free(out, FALSE);
}

static gchar *tool_export_message(const McpJson *args, gchar **error)
{
	FolderItem *item = require_folder(args, "folder", error);
	MsgInfo *msginfo;
	gchar *dest, *dir, *file, *contents = NULL;
	gsize len = 0;
	GString *out;

	if (item == NULL)
		return NULL;
	dest = resolve_user_path(mcp_json_member_string(args, "path", NULL), error);
	if (dest == NULL)
		return NULL;
	msginfo = require_msginfo(args, item, error);
	if (msginfo == NULL) {
		g_free(dest);
		return NULL;
	}

	file = procmsg_get_message_file(msginfo);
	MCP_MSGINFO_FREE(msginfo);
	if (file == NULL || !g_file_get_contents(file, &contents, &len, NULL)) {
		fail_with(error, "could not read message");
		g_free(file);
		g_free(dest);
		return NULL;
	}
	g_free(file);

	dir = g_path_get_dirname(dest);
	g_mkdir_with_parents(dir, 0700);
	g_free(dir);
	if (!g_file_set_contents(dest, contents, (gssize)len, NULL)) {
		fail_with(error, "could not write to %s", dest);
		g_free(contents);
		g_free(dest);
		return NULL;
	}
	g_free(contents);

	out = g_string_new("{");
	jstr(out, "path", dest);
	jint(out, "exported_bytes", (gint64)len);
	g_string_append_c(out, '}');
	g_free(dest);
	return g_string_free(out, FALSE);
}

static gchar *tool_import_message(const McpJson *args, gchar **error)
{
	FolderItem *item = require_folder(args, "folder", error);
	MsgFlags flags;
	gchar *path;
	gint newnum;

	if (item == NULL)
		return NULL;
	if (item->no_select) {
		fail_with(error, "target folder is not selectable");
		return NULL;
	}
	path = resolve_user_path(mcp_json_member_string(args, "path", NULL), error);
	if (path == NULL)
		return NULL;
	if (!g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
		fail_with(error, "file not found: %s", path);
		g_free(path);
		return NULL;
	}

	flags.perm_flags = MSG_NEW | MSG_UNREAD;
	flags.tmp_flags = 0;
	newnum = folder_item_add_msg(item, path, &flags, FALSE);
	g_free(path);
	if (newnum < 0) {
		fail_with(error, "import failed");
		return NULL;
	}
	return g_strdup_printf("{\"msgnum\":%d}", newnum);
}

typedef struct {
	MsgInfo  *msginfo;
	MimeInfo *root;
	MimeInfo *part;
	gint      index;
} PartRef;

static void part_ref_clear(PartRef *ref)
{
	if (ref->root != NULL)
		procmime_mimeinfo_free_all(&ref->root);
	if (ref->msginfo != NULL)
		MCP_MSGINFO_FREE(ref->msginfo);
}

static gboolean require_part(const McpJson *args, PartRef *ref, gchar **error)
{
	FolderItem *item = require_folder(args, "folder", error);
	MimeInfo *cur;
	gint index = 0;

	memset(ref, 0, sizeof *ref);
	ref->index = (gint)mcp_json_member_int(args, "part", -1);
	if (item == NULL)
		return FALSE;
	if (ref->index < 0) {
		fail_with(error, "part is required (see attachments in read_message)");
		return FALSE;
	}
	ref->msginfo = require_msginfo(args, item, error);
	if (ref->msginfo == NULL)
		return FALSE;
	ref->root = procmime_scan_message(ref->msginfo);
	for (cur = ref->root; cur != NULL; cur = procmime_mimeinfo_next(cur), index++) {
		if (index == ref->index) {
			if (mcp_part_is_attachment(cur))
				ref->part = cur;
			break;
		}
	}
	if (ref->part == NULL) {
		fail_with(error, "part %d is not an attachment of this message", ref->index);
		part_ref_clear(ref);
		return FALSE;
	}
	if (!procmime_decode_content(ref->part)) {
		fail_with(error, "could not decode part %d", ref->index);
		part_ref_clear(ref);
		return FALSE;
	}
	return TRUE;
}

static gchar *tool_get_attachment(const McpJson *args, gchar **error)
{
	gint64 max_bytes = mcp_json_member_int(args, "max_bytes", MCP_DEFAULT_INLINE_BYTES);
	PartRef ref;
	gchar *type, *encoded;
	gpointer data;
	gsize size;
	GString *out;

	if (max_bytes <= 0)
		max_bytes = MCP_DEFAULT_INLINE_BYTES;
	if (max_bytes > MCP_MAX_INLINE_BYTES)
		max_bytes = MCP_MAX_INLINE_BYTES;
	if (!require_part(args, &ref, error))
		return NULL;

	size = ref.part->length;
	if ((gint64)size > max_bytes) {
		fail_with(error, "attachment is %" G_GSIZE_FORMAT " bytes, above max_bytes %"
			  G_GINT64_FORMAT "; use save_attachment", size, max_bytes);
		part_ref_clear(&ref);
		return NULL;
	}
	data = procmime_get_part_as_string(ref.part, FALSE);
	if (data == NULL && size > 0) {
		fail_with(error, "could not read part %d", ref.index);
		part_ref_clear(&ref);
		return NULL;
	}

	encoded = g_base64_encode(data, size);
	type = procmime_get_content_type_str(ref.part->type, ref.part->subtype);
	out = g_string_new("{");
	jint(out, "part", ref.index);
	jstr(out, "filename", mcp_part_filename(ref.part));
	jstr(out, "content_type", type);
	jint(out, "size", (gint64)size);
	jstr(out, "data_base64", encoded);
	g_string_append_c(out, '}');

	g_free(type);
	g_free(encoded);
	g_free(data);
	part_ref_clear(&ref);
	return g_string_free(out, FALSE);
}

static gchar *tool_save_attachment(const McpJson *args, gchar **error)
{
	PartRef ref;
	gchar *root, *dir, *name, *path, *raw;
	GStatBuf st;
	GString *out;

	if (!require_part(args, &ref, error))
		return NULL;
	root = file_root(error);
	if (root == NULL) {
		part_ref_clear(&ref);
		return NULL;
	}
	dir = g_build_filename(root, "attachments", NULL);
	g_free(root);
	if (g_mkdir_with_parents(dir, 0700) != 0) {
		fail_with(error, "cannot create %s", dir);
		g_free(dir);
		part_ref_clear(&ref);
		return NULL;
	}

	raw = mcp_part_filename(ref.part) != NULL
		? g_strdup(mcp_part_filename(ref.part))
		: g_strdup_printf("attachment-%d", ref.index);
	name = mcp_safe_filename(raw);
	g_free(raw);
	path = mcp_unique_path(dir, name);
	g_free(dir);

	if (path == NULL || procmime_get_part(path, ref.part) != 0) {
		fail_with(error, "could not save part %d", ref.index);
		g_free(path);
		g_free(name);
		part_ref_clear(&ref);
		return NULL;
	}

	out = g_string_new("{");
	jstr(out, "path", path);
	jstr(out, "filename", name);
	jint(out, "size", g_stat(path, &st) == 0 ? (gint64)st.st_size : -1);
	g_string_append_c(out, '}');

	g_free(path);
	g_free(name);
	part_ref_clear(&ref);
	return g_string_free(out, FALSE);
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
		"List messages of a folder, newest first, with Claws flags, tags and color label. "
		"Returns {messages, total, offset, returned, has_more}; total is omitted when filtering by has_attachment.",
		"{\"type\":\"object\",\"properties\":{"
		"\"folder\":{\"type\":\"string\",\"description\":\"folder identifier (#mh/Mail/inbox) or name; defaults to inbox\"},"
		"\"limit\":{\"type\":\"integer\",\"description\":\"max messages (default 20, max 500)\"},"
		"\"offset\":{\"type\":\"integer\",\"description\":\"skip this many matching messages\"},"
		"\"unread_only\":{\"type\":\"boolean\",\"description\":\"same as unread=true\"},"
		FILTER_PROPS
		"}}",
		tool_list_messages, FALSE
	},
	{
		"read_message",
		"Read a single message: headers, flags, tags, attachment list and its first text part.",
		"{\"type\":\"object\",\"properties\":{"
		"\"folder\":{\"type\":\"string\"},"
		"\"msgnum\":{\"type\":\"integer\",\"description\":\"message number as returned by list_messages\"},"
		"\"max_chars\":{\"type\":\"integer\",\"description\":\"truncate body to this many bytes (default 20000)\"}"
		"},\"required\":[\"msgnum\"]}",
		tool_read_message, FALSE
	},
	{
		"search_messages",
		"Case-insensitive substring search over a folder (headers, optionally body), narrowed by optional filters.",
		"{\"type\":\"object\",\"properties\":{"
		"\"query\":{\"type\":\"string\"},"
		"\"folder\":{\"type\":\"string\"},"
		"\"limit\":{\"type\":\"integer\"},"
		"\"include_body\":{\"type\":\"boolean\",\"description\":\"also scan the text body (slower)\"},"
		"\"max_scan\":{\"type\":\"integer\"},"
		FILTER_PROPS
		"},\"required\":[\"query\"]}",
		tool_search_messages, FALSE
	},
	{
		"get_attachment",
		"Return one attachment's decoded content as base64. Part numbers come from read_message.",
		"{\"type\":\"object\",\"properties\":{"
		"\"folder\":{\"type\":\"string\"},"
		"\"msgnum\":{\"type\":\"integer\"},"
		"\"part\":{\"type\":\"integer\",\"description\":\"part number from read_message attachments\"},"
		"\"max_bytes\":{\"type\":\"integer\",\"description\":\"refuse larger attachments (default 1 MiB, max 8 MiB)\"}"
		"},\"required\":[\"msgnum\",\"part\"]}",
		tool_get_attachment, FALSE
	},
	{
		"save_attachment",
		"Save one attachment into the attachments subfolder of file_dir and return its path. Never overwrites.",
		"{\"type\":\"object\",\"properties\":{"
		"\"folder\":{\"type\":\"string\"},"
		"\"msgnum\":{\"type\":\"integer\"},"
		"\"part\":{\"type\":\"integer\",\"description\":\"part number from read_message attachments\"}"
		"},\"required\":[\"msgnum\",\"part\"]}",
		tool_save_attachment, TRUE
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
		"\"attach\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"files inside file_dir, absolute or relative to it\"}"
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
		"Set or clear a message flag. Setting spam does not train a spam filter; use learn_spam for that.",
		"{\"type\":\"object\",\"properties\":{"
		"\"folder\":{\"type\":\"string\"},"
		"\"msgnum\":{\"type\":\"integer\"},"
		"\"flag\":{\"type\":\"string\",\"enum\":" MCP_FLAG_ENUM_JSON "},"
		"\"value\":{\"type\":\"boolean\",\"description\":\"true sets the flag (default), false clears it\"}"
		"},\"required\":[\"msgnum\",\"flag\"]}",
		tool_set_flag, TRUE
	},
	{
		"set_flags",
		"Set or clear one flag on many messages of a folder at once.",
		"{\"type\":\"object\",\"properties\":{"
		"\"folder\":{\"type\":\"string\"},"
		MSGNUMS_PROP ","
		"\"flag\":{\"type\":\"string\",\"enum\":" MCP_FLAG_ENUM_JSON "},"
		"\"value\":{\"type\":\"boolean\",\"description\":\"true sets the flag (default), false clears it\"}"
		"},\"required\":[\"msgnums\",\"flag\"]}",
		tool_set_flags, TRUE
	},
	{
		"move_messages",
		"Move many messages of a folder to another folder in one step.",
		"{\"type\":\"object\",\"properties\":{"
		"\"folder\":{\"type\":\"string\"},"
		MSGNUMS_PROP ","
		"\"target\":{\"type\":\"string\"}"
		"},\"required\":[\"msgnums\",\"target\"]}",
		tool_move_messages, TRUE
	},
	{
		"learn_spam",
		"Train the active spam filter plugin (Bogofilter or SpamAssassin) with messages and set or clear their spam flag.",
		"{\"type\":\"object\",\"properties\":{"
		"\"folder\":{\"type\":\"string\"},"
		MSGNUMS_PROP ","
		"\"spam\":{\"type\":\"boolean\",\"description\":\"true learns as spam (default), false learns as not spam\"}"
		"},\"required\":[\"msgnums\"]}",
		tool_learn_spam, TRUE
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
		"Write a message's raw source to a file inside file_dir.",
		"{\"type\":\"object\",\"properties\":{"
		"\"folder\":{\"type\":\"string\"},"
		"\"msgnum\":{\"type\":\"integer\"},"
		"\"path\":{\"type\":\"string\",\"description\":\"destination inside file_dir, absolute or relative to it\"}"
		"},\"required\":[\"msgnum\",\"path\"]}",
		tool_export_message, TRUE
	},
	{
		"import_message",
		"Add an RFC822 (.eml) file from file_dir into a folder as a new message.",
		"{\"type\":\"object\",\"properties\":{"
		"\"folder\":{\"type\":\"string\"},"
		"\"path\":{\"type\":\"string\",\"description\":\".eml file inside file_dir, absolute or relative to it\"}"
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
