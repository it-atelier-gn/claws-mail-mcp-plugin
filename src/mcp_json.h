/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Georg Nelles */

#ifndef MCP_JSON_H
#define MCP_JSON_H

#include <glib.h>

typedef enum {
	MCP_JSON_NULL,
	MCP_JSON_BOOL,
	MCP_JSON_NUMBER,
	MCP_JSON_STRING,
	MCP_JSON_ARRAY,
	MCP_JSON_OBJECT
} McpJsonType;

typedef struct McpJsonImpl McpJson;

McpJson     *mcp_json_parse(const gchar *text, gssize len, GError **error);
McpJson     *mcp_json_copy(const McpJson *node);
void         mcp_json_free(McpJson *node);

McpJsonType  mcp_json_type(const McpJson *node);
gboolean     mcp_json_is(const McpJson *node, McpJsonType type);

const McpJson *mcp_json_member(const McpJson *node, const gchar *key);
guint          mcp_json_length(const McpJson *node);
const McpJson *mcp_json_index(const McpJson *node, guint i);

const gchar *mcp_json_get_string(const McpJson *node, const gchar *fallback);
gint64       mcp_json_get_int(const McpJson *node, gint64 fallback);
gdouble      mcp_json_get_double(const McpJson *node, gdouble fallback);
gboolean     mcp_json_get_bool(const McpJson *node, gboolean fallback);

const gchar *mcp_json_member_string(const McpJson *node, const gchar *key, const gchar *fallback);
gint64       mcp_json_member_int(const McpJson *node, const gchar *key, gint64 fallback);
gboolean     mcp_json_member_bool(const McpJson *node, const gchar *key, gboolean fallback);

void   mcp_json_write(const McpJson *node, GString *out);
gchar *mcp_json_to_string(const McpJson *node);

void   mcp_json_escape(GString *out, const gchar *text);
gchar *mcp_json_quote(const gchar *text);

#endif
