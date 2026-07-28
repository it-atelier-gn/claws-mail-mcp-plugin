/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Georg Nelles */

#ifndef MCP_HTTP_H
#define MCP_HTTP_H

#include <glib.h>

typedef struct {
	const gchar *method;
	const gchar *path;
	const gchar *authorization;
	const gchar *origin;
	const gchar *content_type;
	const gchar *accept;
	const gchar *session_id;
	const gchar *body;
	gsize        body_len;
} McpHttpRequest;

typedef struct {
	gint   status;
	gchar *content_type;
	gchar *body;
	gchar *extra_headers;
} McpHttpResponse;

typedef void (*McpHttpHandler)(const McpHttpRequest *req, McpHttpResponse *res,
			       gpointer user_data);

typedef struct McpHttpServerImpl McpHttpServer;

McpHttpServer *mcp_http_server_start(const gchar *bind_address, guint16 port,
				     McpHttpHandler handler, gpointer user_data,
				     GError **error);
void           mcp_http_server_stop(McpHttpServer *server);
guint16        mcp_http_server_port(McpHttpServer *server);

#endif
