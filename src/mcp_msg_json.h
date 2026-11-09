/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Georg Nelles */

#ifndef MCP_MSG_JSON_H
#define MCP_MSG_JSON_H

#include "claws-features.h"

#include <stdio.h>
#include <string.h>
#include <glib.h>

#include "procmsg.h"
#include "procmime.h"

#include "mcp_json.h"

#define MCP_ANY (-1)

typedef struct {
	gint         unread;
	gint         spam;
	gint         marked;
	gint         has_attachment;
	const gchar *tag;
	gint64       since;
	gint64       until;
} McpMsgFilter;

static inline void jkey(GString *out, const gchar *key)
{
	if (out->len > 0 && out->str[out->len - 1] != '{' && out->str[out->len - 1] != '[')
		g_string_append_c(out, ',');
	mcp_json_escape(out, key);
	g_string_append_c(out, ':');
}

static inline void jstr(GString *out, const gchar *key, const gchar *value)
{
	jkey(out, key);
	mcp_json_escape(out, value != NULL ? value : "");
}

static inline void jint(GString *out, const gchar *key, gint64 value)
{
	jkey(out, key);
	g_string_append_printf(out, "%" G_GINT64_FORMAT, value);
}

static inline void jbool(GString *out, const gchar *key, gboolean value)
{
	jkey(out, key);
	g_string_append(out, value ? "true" : "false");
}

static inline void jsep(GString *out)
{
	if (out->len > 0 && out->str[out->len - 1] != '[')
		g_string_append_c(out, ',');
}

static inline void jstrv(GString *out, const gchar *key, const gchar *const *values)
{
	gsize i;

	jkey(out, key);
	g_string_append_c(out, '[');
	for (i = 0; values != NULL && values[i] != NULL; i++) {
		if (i > 0)
			g_string_append_c(out, ',');
		mcp_json_escape(out, values[i]);
	}
	g_string_append_c(out, ']');
}

static inline gchar *fmt_time(time_t t)
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

static inline gboolean mcp_parse_day(const gchar *text, gint64 *out)
{
	gint y, m, d;
	gchar tail;
	GDateTime *dt;

	if (text == NULL || strlen(text) != 10 || text[4] != '-' || text[7] != '-')
		return FALSE;
	if (sscanf(text, "%4d-%2d-%2d%c", &y, &m, &d, &tail) != 3)
		return FALSE;
	if (!g_date_valid_dmy((GDateDay)d, (GDateMonth)m, (GDateYear)y))
		return FALSE;
	dt = g_date_time_new_local(y, m, d, 0, 0, 0);
	if (dt == NULL)
		return FALSE;
	*out = g_date_time_to_unix(dt);
	g_date_time_unref(dt);
	return TRUE;
}

static inline gint mcp_tri_member(const McpJson *args, const gchar *key)
{
	const McpJson *node = mcp_json_member(args, key);

	if (!mcp_json_is(node, MCP_JSON_BOOL))
		return MCP_ANY;
	return mcp_json_get_bool(node, FALSE) ? 1 : 0;
}

static inline gboolean mcp_filter_parse(const McpJson *args, McpMsgFilter *f, gchar **error)
{
	const gchar *since = mcp_json_member_string(args, "since", NULL);
	const gchar *until = mcp_json_member_string(args, "until", NULL);

	f->unread = mcp_tri_member(args, "unread");
	if (f->unread == MCP_ANY && mcp_json_member_bool(args, "unread_only", FALSE))
		f->unread = 1;
	f->spam = mcp_tri_member(args, "spam");
	f->marked = mcp_tri_member(args, "marked");
	f->has_attachment = mcp_tri_member(args, "has_attachment");
	f->tag = mcp_json_member_string(args, "tag", NULL);
	if (f->tag != NULL && *f->tag == '\0')
		f->tag = NULL;
	f->since = 0;
	f->until = 0;

	if (since != NULL && *since != '\0' && !mcp_parse_day(since, &f->since)) {
		*error = g_strdup_printf("since must be YYYY-MM-DD: %s", since);
		return FALSE;
	}
	if (until != NULL && *until != '\0') {
		if (!mcp_parse_day(until, &f->until)) {
			*error = g_strdup_printf("until must be YYYY-MM-DD: %s", until);
			return FALSE;
		}
		f->until += 24 * 60 * 60;
	}
	return TRUE;
}

static inline gboolean mcp_tri_ok(gint want, gboolean have)
{
	return want == MCP_ANY || (want == 1) == (have != FALSE);
}

static inline gboolean mcp_filter_matches(const McpMsgFilter *f, const MsgInfo *msginfo,
					  const gchar *const *tags)
{
	gsize i;
	gboolean tag_hit;

	if (!mcp_tri_ok(f->unread, MSG_IS_UNREAD(msginfo->flags)) ||
	    !mcp_tri_ok(f->spam, MSG_IS_SPAM(msginfo->flags)) ||
	    !mcp_tri_ok(f->marked, MSG_IS_MARKED(msginfo->flags)))
		return FALSE;
	if (f->since != 0 && (gint64)msginfo->date_t < f->since)
		return FALSE;
	if (f->until != 0 && (gint64)msginfo->date_t >= f->until)
		return FALSE;
	if (f->tag == NULL)
		return TRUE;
	tag_hit = FALSE;
	for (i = 0; tags != NULL && tags[i] != NULL && !tag_hit; i++)
		tag_hit = g_utf8_collate(tags[i], f->tag) == 0 ||
			  g_ascii_strcasecmp(tags[i], f->tag) == 0;
	return tag_hit;
}

static inline gboolean mcp_flag_from_name(const gchar *name, guint *bit)
{
	static const struct { const gchar *name; guint bit; } map[] = {
		{ "marked", MSG_MARKED },
		{ "locked", MSG_LOCKED },
		{ "unread", MSG_UNREAD },
		{ "replied", MSG_REPLIED },
		{ "forwarded", MSG_FORWARDED },
		{ "ignore_thread", MSG_IGNORE_THREAD },
		{ "watch_thread", MSG_WATCH_THREAD },
		{ "spam", MSG_SPAM },
	};
	gsize i;

	if (name == NULL)
		return FALSE;
	for (i = 0; i < G_N_ELEMENTS(map); i++) {
		if (g_ascii_strcasecmp(name, map[i].name) == 0) {
			*bit = map[i].bit;
			return TRUE;
		}
	}
	return FALSE;
}

#define MCP_FLAG_ENUM_JSON \
	"[\"marked\",\"locked\",\"unread\",\"replied\",\"forwarded\"," \
	"\"ignore_thread\",\"watch_thread\",\"spam\"]"

static inline void append_msg_fields(GString *out, const MsgInfo *msginfo, const gchar *folder_id,
				     const gchar *const *tags, gboolean has_attachment)
{
	gchar *date = fmt_time(msginfo->date_t);

	jint(out, "msgnum", msginfo->msgnum);
	jstr(out, "folder", folder_id);
	jstr(out, "date", date);
	jstr(out, "from", msginfo->from);
	jstr(out, "to", msginfo->to);
	jstr(out, "subject", msginfo->subject);
	jstr(out, "message_id", msginfo->msgid);
	jstr(out, "in_reply_to", msginfo->inreplyto);
	jint(out, "size", (gint64)msginfo->size);
	jbool(out, "unread", MSG_IS_UNREAD(msginfo->flags));
	jbool(out, "new", MSG_IS_NEW(msginfo->flags));
	jbool(out, "marked", MSG_IS_MARKED(msginfo->flags));
	jbool(out, "replied", MSG_IS_REPLIED(msginfo->flags));
	jbool(out, "forwarded", MSG_IS_FORWARDED(msginfo->flags));
	jbool(out, "locked", MSG_IS_LOCKED(msginfo->flags));
	jbool(out, "ignore_thread", MSG_IS_IGNORE_THREAD(msginfo->flags));
	jbool(out, "watch_thread", MSG_IS_WATCH_THREAD(msginfo->flags));
	jbool(out, "spam", MSG_IS_SPAM(msginfo->flags));
	jbool(out, "signed", MSG_IS_SIGNED(msginfo->flags));
	jbool(out, "encrypted", MSG_IS_ENCRYPTED(msginfo->flags));
	jbool(out, "has_attachment", has_attachment);
	jint(out, "color_label", (gint64)MSG_GET_COLORLABEL_VALUE(msginfo->flags));
	jstrv(out, "tags", tags);
	g_free(date);
}

static inline void append_msg_summary(GString *out, const MsgInfo *msginfo, const gchar *folder_id,
				      const gchar *const *tags, gboolean has_attachment)
{
	g_string_append_c(out, '{');
	append_msg_fields(out, msginfo, folder_id, tags, has_attachment);
	g_string_append_c(out, '}');
}

static inline const gchar *mcp_part_param(GHashTable *params, const gchar *key)
{
	const gchar *value;

	if (params == NULL)
		return NULL;
	value = g_hash_table_lookup(params, key);
	return value != NULL && *value != '\0' ? value : NULL;
}

static inline const gchar *mcp_part_filename(const MimeInfo *mimeinfo)
{
	const gchar *name = mcp_part_param(mimeinfo->dispositionparameters, "filename");

	if (name == NULL)
		name = mcp_part_param(mimeinfo->typeparameters, "name");
	if (name == NULL)
		name = mcp_part_param(mimeinfo->dispositionparameters, "name");
	return name;
}

static inline gboolean mcp_part_is_attachment(const MimeInfo *mimeinfo)
{
	const gchar *sub = mimeinfo->subtype != NULL ? mimeinfo->subtype : "";

	if (mimeinfo->node != NULL && !G_NODE_IS_LEAF(mimeinfo->node))
		return FALSE;
	if (mimeinfo->type == MIMETYPE_MULTIPART)
		return FALSE;
	if (mimeinfo->disposition == DISPOSITIONTYPE_ATTACHMENT)
		return TRUE;
	if (mcp_part_filename(mimeinfo) != NULL)
		return TRUE;
	if (mimeinfo->type == MIMETYPE_TEXT)
		return FALSE;
	if (mimeinfo->type == MIMETYPE_APPLICATION &&
	    (g_ascii_strcasecmp(sub, "pgp-signature") == 0 ||
	     g_ascii_strcasecmp(sub, "pkcs7-signature") == 0 ||
	     g_ascii_strcasecmp(sub, "x-pkcs7-signature") == 0 ||
	     g_ascii_strcasecmp(sub, "pgp-encrypted") == 0))
		return FALSE;
	return TRUE;
}

#endif
