/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Georg Nelles */

#include <string.h>
#include <stdlib.h>

#include "mcp_json.h"

#define MCP_JSON_MAX_DEPTH 64

struct McpJsonImpl {
	McpJsonType type;
	gchar      *key;
	gchar      *str;
	gdouble     num;
	gboolean    bval;
	GPtrArray  *items;
};

typedef struct {
	const gchar *p;
	const gchar *end;
	gint         depth;
} ParseCtx;

static McpJson *parse_value(ParseCtx *ctx, GError **error);

static McpJson *node_new(McpJsonType type)
{
	McpJson *n = g_new0(McpJson, 1);
	n->type = type;
	return n;
}

static GPtrArray *items_new(void)
{
	return g_ptr_array_new_with_free_func((GDestroyNotify)mcp_json_free);
}

void mcp_json_free(McpJson *node)
{
	if (node == NULL)
		return;
	g_free(node->key);
	g_free(node->str);
	if (node->items != NULL)
		g_ptr_array_free(node->items, TRUE);
	g_free(node);
}

McpJson *mcp_json_copy(const McpJson *node)
{
	McpJson *copy;
	guint i;

	if (node == NULL)
		return NULL;

	copy = node_new(node->type);
	copy->num = node->num;
	copy->bval = node->bval;
	copy->key = g_strdup(node->key);
	copy->str = g_strdup(node->str);
	if (node->items != NULL) {
		copy->items = items_new();
		for (i = 0; i < node->items->len; i++)
			g_ptr_array_add(copy->items,
					mcp_json_copy(g_ptr_array_index(node->items, i)));
	}
	return copy;
}

static void fail(GError **error, const gchar *what)
{
	g_set_error(error, g_quark_from_static_string("mcp-json"), 1, "%s", what);
}

static void skip_ws(ParseCtx *ctx)
{
	while (ctx->p < ctx->end) {
		gchar c = *ctx->p;
		if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
			ctx->p++;
		else
			break;
	}
}

static gboolean at(ParseCtx *ctx, gchar c)
{
	return ctx->p < ctx->end && *ctx->p == c;
}

static gboolean read_hex4(ParseCtx *ctx, guint *out)
{
	guint v = 0;
	gint i;

	if (ctx->end - ctx->p < 4)
		return FALSE;
	for (i = 0; i < 4; i++) {
		gchar c = ctx->p[i];
		v <<= 4;
		if (c >= '0' && c <= '9')
			v |= (guint)(c - '0');
		else if (c >= 'a' && c <= 'f')
			v |= (guint)(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F')
			v |= (guint)(c - 'A' + 10);
		else
			return FALSE;
	}
	ctx->p += 4;
	*out = v;
	return TRUE;
}

static gchar *parse_string_raw(ParseCtx *ctx, GError **error)
{
	GString *s;

	if (!at(ctx, '"')) {
		fail(error, "expected string");
		return NULL;
	}
	ctx->p++;
	s = g_string_new(NULL);

	while (ctx->p < ctx->end) {
		guchar c = (guchar)*ctx->p;

		if (c == '"') {
			ctx->p++;
			return g_string_free(s, FALSE);
		}
		if (c == '\\') {
			ctx->p++;
			if (ctx->p >= ctx->end)
				break;
			switch (*ctx->p) {
			case '"':  g_string_append_c(s, '"');  ctx->p++; break;
			case '\\': g_string_append_c(s, '\\'); ctx->p++; break;
			case '/':  g_string_append_c(s, '/');  ctx->p++; break;
			case 'b':  g_string_append_c(s, '\b'); ctx->p++; break;
			case 'f':  g_string_append_c(s, '\f'); ctx->p++; break;
			case 'n':  g_string_append_c(s, '\n'); ctx->p++; break;
			case 'r':  g_string_append_c(s, '\r'); ctx->p++; break;
			case 't':  g_string_append_c(s, '\t'); ctx->p++; break;
			case 'u': {
				guint cp = 0, low = 0;
				gchar buf[8];
				gint len;

				ctx->p++;
				if (!read_hex4(ctx, &cp)) {
					fail(error, "bad \\u escape");
					g_string_free(s, TRUE);
					return NULL;
				}
				if (cp >= 0xD800 && cp <= 0xDBFF &&
				    ctx->end - ctx->p >= 6 &&
				    ctx->p[0] == '\\' && ctx->p[1] == 'u') {
					const gchar *save = ctx->p;
					ctx->p += 2;
					if (read_hex4(ctx, &low) &&
					    low >= 0xDC00 && low <= 0xDFFF)
						cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
					else
						ctx->p = save;
				}
				if (cp >= 0xD800 && cp <= 0xDFFF)
					cp = 0xFFFD;
				len = g_unichar_to_utf8((gunichar)cp, buf);
				g_string_append_len(s, buf, len);
				break;
			}
			default:
				fail(error, "unknown escape");
				g_string_free(s, TRUE);
				return NULL;
			}
			continue;
		}
		if (c < 0x20) {
			fail(error, "control character in string");
			g_string_free(s, TRUE);
			return NULL;
		}
		g_string_append_c(s, (gchar)c);
		ctx->p++;
	}

	fail(error, "unterminated string");
	g_string_free(s, TRUE);
	return NULL;
}

static McpJson *parse_number(ParseCtx *ctx, GError **error)
{
	const gchar *start = ctx->p;
	gchar *tail = NULL;
	gchar *buf;
	McpJson *n;
	gdouble v;

	if (at(ctx, '-'))
		ctx->p++;
	while (ctx->p < ctx->end && (g_ascii_isdigit(*ctx->p) || *ctx->p == '.' ||
				     *ctx->p == 'e' || *ctx->p == 'E' ||
				     *ctx->p == '+' || *ctx->p == '-'))
		ctx->p++;

	buf = g_strndup(start, (gsize)(ctx->p - start));
	v = g_ascii_strtod(buf, &tail);
	if (tail == buf || (tail != NULL && *tail != '\0')) {
		fail(error, "invalid number");
		g_free(buf);
		return NULL;
	}
	g_free(buf);

	n = node_new(MCP_JSON_NUMBER);
	n->num = v;
	return n;
}

static McpJson *parse_object(ParseCtx *ctx, GError **error)
{
	McpJson *obj = node_new(MCP_JSON_OBJECT);

	obj->items = items_new();
	ctx->p++;
	skip_ws(ctx);
	if (at(ctx, '}')) {
		ctx->p++;
		return obj;
	}

	for (;;) {
		gchar *key;
		McpJson *val;

		skip_ws(ctx);
		key = parse_string_raw(ctx, error);
		if (key == NULL)
			goto err;
		skip_ws(ctx);
		if (!at(ctx, ':')) {
			g_free(key);
			fail(error, "expected ':'");
			goto err;
		}
		ctx->p++;
		val = parse_value(ctx, error);
		if (val == NULL) {
			g_free(key);
			goto err;
		}
		val->key = key;
		g_ptr_array_add(obj->items, val);

		skip_ws(ctx);
		if (at(ctx, ',')) {
			ctx->p++;
			continue;
		}
		if (at(ctx, '}')) {
			ctx->p++;
			return obj;
		}
		fail(error, "expected ',' or '}'");
		goto err;
	}

err:
	mcp_json_free(obj);
	return NULL;
}

static McpJson *parse_array(ParseCtx *ctx, GError **error)
{
	McpJson *arr = node_new(MCP_JSON_ARRAY);

	arr->items = items_new();
	ctx->p++;
	skip_ws(ctx);
	if (at(ctx, ']')) {
		ctx->p++;
		return arr;
	}

	for (;;) {
		McpJson *val = parse_value(ctx, error);

		if (val == NULL)
			goto err;
		g_ptr_array_add(arr->items, val);

		skip_ws(ctx);
		if (at(ctx, ',')) {
			ctx->p++;
			continue;
		}
		if (at(ctx, ']')) {
			ctx->p++;
			return arr;
		}
		fail(error, "expected ',' or ']'");
		goto err;
	}

err:
	mcp_json_free(arr);
	return NULL;
}

static McpJson *parse_value(ParseCtx *ctx, GError **error)
{
	McpJson *n;

	if (++ctx->depth > MCP_JSON_MAX_DEPTH) {
		fail(error, "nesting too deep");
		ctx->depth--;
		return NULL;
	}
	skip_ws(ctx);
	if (ctx->p >= ctx->end) {
		fail(error, "unexpected end of input");
		ctx->depth--;
		return NULL;
	}

	switch (*ctx->p) {
	case '{':
		n = parse_object(ctx, error);
		break;
	case '[':
		n = parse_array(ctx, error);
		break;
	case '"': {
		gchar *s = parse_string_raw(ctx, error);
		if (s == NULL) {
			n = NULL;
		} else {
			n = node_new(MCP_JSON_STRING);
			n->str = s;
		}
		break;
	}
	case 't':
		if (ctx->end - ctx->p >= 4 && strncmp(ctx->p, "true", 4) == 0) {
			ctx->p += 4;
			n = node_new(MCP_JSON_BOOL);
			n->bval = TRUE;
		} else {
			fail(error, "invalid literal");
			n = NULL;
		}
		break;
	case 'f':
		if (ctx->end - ctx->p >= 5 && strncmp(ctx->p, "false", 5) == 0) {
			ctx->p += 5;
			n = node_new(MCP_JSON_BOOL);
			n->bval = FALSE;
		} else {
			fail(error, "invalid literal");
			n = NULL;
		}
		break;
	case 'n':
		if (ctx->end - ctx->p >= 4 && strncmp(ctx->p, "null", 4) == 0) {
			ctx->p += 4;
			n = node_new(MCP_JSON_NULL);
		} else {
			fail(error, "invalid literal");
			n = NULL;
		}
		break;
	default:
		if (*ctx->p == '-' || g_ascii_isdigit(*ctx->p))
			n = parse_number(ctx, error);
		else {
			fail(error, "unexpected character");
			n = NULL;
		}
		break;
	}

	ctx->depth--;
	return n;
}

McpJson *mcp_json_parse(const gchar *text, gssize len, GError **error)
{
	ParseCtx ctx;
	McpJson *root;

	if (text == NULL) {
		fail(error, "no input");
		return NULL;
	}
	ctx.p = text;
	ctx.end = text + (len < 0 ? (gssize)strlen(text) : len);
	ctx.depth = 0;

	root = parse_value(&ctx, error);
	if (root == NULL)
		return NULL;

	skip_ws(&ctx);
	if (ctx.p != ctx.end) {
		fail(error, "trailing data after JSON value");
		mcp_json_free(root);
		return NULL;
	}
	return root;
}

McpJsonType mcp_json_type(const McpJson *node)
{
	return node != NULL ? node->type : MCP_JSON_NULL;
}

gboolean mcp_json_is(const McpJson *node, McpJsonType type)
{
	return node != NULL && node->type == type;
}

const McpJson *mcp_json_member(const McpJson *node, const gchar *key)
{
	guint i;

	if (!mcp_json_is(node, MCP_JSON_OBJECT) || key == NULL)
		return NULL;
	for (i = 0; i < node->items->len; i++) {
		McpJson *m = g_ptr_array_index(node->items, i);
		if (m->key != NULL && strcmp(m->key, key) == 0)
			return m;
	}
	return NULL;
}

guint mcp_json_length(const McpJson *node)
{
	if (node == NULL || node->items == NULL)
		return 0;
	return node->items->len;
}

const McpJson *mcp_json_index(const McpJson *node, guint i)
{
	if (node == NULL || node->items == NULL || i >= node->items->len)
		return NULL;
	return g_ptr_array_index(node->items, i);
}

const gchar *mcp_json_get_string(const McpJson *node, const gchar *fallback)
{
	if (!mcp_json_is(node, MCP_JSON_STRING))
		return fallback;
	return node->str;
}

gdouble mcp_json_get_double(const McpJson *node, gdouble fallback)
{
	if (!mcp_json_is(node, MCP_JSON_NUMBER))
		return fallback;
	return node->num;
}

gint64 mcp_json_get_int(const McpJson *node, gint64 fallback)
{
	if (mcp_json_is(node, MCP_JSON_NUMBER))
		return (gint64)node->num;
	if (mcp_json_is(node, MCP_JSON_STRING)) {
		gchar *tail = NULL;
		gint64 v = g_ascii_strtoll(node->str, &tail, 10);
		if (tail != node->str && tail != NULL && *tail == '\0')
			return v;
	}
	return fallback;
}

gboolean mcp_json_get_bool(const McpJson *node, gboolean fallback)
{
	if (!mcp_json_is(node, MCP_JSON_BOOL))
		return fallback;
	return node->bval;
}

const gchar *mcp_json_member_string(const McpJson *node, const gchar *key, const gchar *fallback)
{
	return mcp_json_get_string(mcp_json_member(node, key), fallback);
}

gint64 mcp_json_member_int(const McpJson *node, const gchar *key, gint64 fallback)
{
	return mcp_json_get_int(mcp_json_member(node, key), fallback);
}

gboolean mcp_json_member_bool(const McpJson *node, const gchar *key, gboolean fallback)
{
	return mcp_json_get_bool(mcp_json_member(node, key), fallback);
}

void mcp_json_escape(GString *out, const gchar *text)
{
	const guchar *p;

	if (text == NULL)
		text = "";
	g_string_append_c(out, '"');
	for (p = (const guchar *)text; *p != '\0'; p++) {
		switch (*p) {
		case '"':  g_string_append(out, "\\\""); break;
		case '\\': g_string_append(out, "\\\\"); break;
		case '\b': g_string_append(out, "\\b"); break;
		case '\f': g_string_append(out, "\\f"); break;
		case '\n': g_string_append(out, "\\n"); break;
		case '\r': g_string_append(out, "\\r"); break;
		case '\t': g_string_append(out, "\\t"); break;
		default:
			if (*p < 0x20)
				g_string_append_printf(out, "\\u%04x", *p);
			else
				g_string_append_c(out, (gchar)*p);
			break;
		}
	}
	g_string_append_c(out, '"');
}

gchar *mcp_json_quote(const gchar *text)
{
	GString *s = g_string_new(NULL);

	mcp_json_escape(s, text);
	return g_string_free(s, FALSE);
}

void mcp_json_write(const McpJson *node, GString *out)
{
	guint i;

	if (node == NULL) {
		g_string_append(out, "null");
		return;
	}

	switch (node->type) {
	case MCP_JSON_NULL:
		g_string_append(out, "null");
		break;
	case MCP_JSON_BOOL:
		g_string_append(out, node->bval ? "true" : "false");
		break;
	case MCP_JSON_NUMBER: {
		gchar buf[G_ASCII_DTOSTR_BUF_SIZE];

		if (node->num == (gdouble)(gint64)node->num &&
		    node->num < 1e15 && node->num > -1e15)
			g_string_append_printf(out, "%" G_GINT64_FORMAT, (gint64)node->num);
		else {
			g_ascii_dtostr(buf, sizeof buf, node->num);
			g_string_append(out, buf);
		}
		break;
	}
	case MCP_JSON_STRING:
		mcp_json_escape(out, node->str);
		break;
	case MCP_JSON_ARRAY:
		g_string_append_c(out, '[');
		for (i = 0; i < mcp_json_length(node); i++) {
			if (i > 0)
				g_string_append_c(out, ',');
			mcp_json_write(mcp_json_index(node, i), out);
		}
		g_string_append_c(out, ']');
		break;
	case MCP_JSON_OBJECT:
		g_string_append_c(out, '{');
		for (i = 0; i < mcp_json_length(node); i++) {
			const McpJson *m = mcp_json_index(node, i);
			if (i > 0)
				g_string_append_c(out, ',');
			mcp_json_escape(out, m->key);
			g_string_append_c(out, ':');
			mcp_json_write(m, out);
		}
		g_string_append_c(out, '}');
		break;
	}
}

gchar *mcp_json_to_string(const McpJson *node)
{
	GString *s = g_string_new(NULL);

	mcp_json_write(node, s);
	return g_string_free(s, FALSE);
}
