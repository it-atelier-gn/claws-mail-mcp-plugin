/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Georg Nelles */

#include <string.h>
#include <gio/gio.h>

#include "mcp_http.h"

#define MCP_HTTP_MAX_HEADER ((gsize)32 * 1024)
#define MCP_HTTP_MAX_BODY   ((gsize)4 * 1024 * 1024)
#define MCP_HTTP_TIMEOUT    30

struct McpHttpServerImpl {
	GSocketService *service;
	McpHttpHandler  handler;
	gpointer        user_data;
	guint16         port;
};

static const gchar *status_text(gint status)
{
	switch (status) {
	case 200: return "OK";
	case 202: return "Accepted";
	case 204: return "No Content";
	case 400: return "Bad Request";
	case 401: return "Unauthorized";
	case 403: return "Forbidden";
	case 404: return "Not Found";
	case 405: return "Method Not Allowed";
	case 408: return "Request Timeout";
	case 413: return "Payload Too Large";
	case 415: return "Unsupported Media Type";
	case 500: return "Internal Server Error";
	default:  return "Error";
	}
}

static void send_response(GOutputStream *out, gint status, const gchar *content_type,
			  const gchar *extra_headers, const gchar *body)
{
	GString *msg = g_string_new(NULL);
	gsize len = body != NULL ? strlen(body) : 0;

	g_string_append_printf(msg, "HTTP/1.1 %d %s\r\n", status, status_text(status));
	g_string_append_printf(msg, "Content-Length: %" G_GSIZE_FORMAT "\r\n", len);
	if (len > 0)
		g_string_append_printf(msg, "Content-Type: %s\r\n",
				       content_type != NULL ? content_type : "application/json");
	if (extra_headers != NULL)
		g_string_append(msg, extra_headers);
	g_string_append(msg, "Cache-Control: no-store\r\n");
	g_string_append(msg, "X-Content-Type-Options: nosniff\r\n");
	g_string_append(msg, "Connection: close\r\n\r\n");
	if (len > 0)
		g_string_append_len(msg, body, len);

	g_output_stream_write_all(out, msg->str, msg->len, NULL, NULL, NULL);
	g_output_stream_flush(out, NULL, NULL);
	g_string_free(msg, TRUE);
}

static void send_error(GOutputStream *out, gint status, const gchar *text)
{
	gchar *body = g_strdup_printf("{\"error\":\"%s\"}", text);

	send_response(out, status, "application/json", NULL, body);
	g_free(body);
}

static gchar *header_lookup(GHashTable *headers, const gchar *name)
{
	return g_hash_table_lookup(headers, name);
}

static gboolean parse_head(const gchar *head, gchar **method, gchar **path,
			   GHashTable *headers)
{
	gchar **lines = g_strsplit(head, "\r\n", -1);
	gchar **first;
	gint i;

	if (lines[0] == NULL) {
		g_strfreev(lines);
		return FALSE;
	}

	first = g_strsplit(lines[0], " ", 3);
	if (first[0] == NULL || first[1] == NULL) {
		g_strfreev(first);
		g_strfreev(lines);
		return FALSE;
	}
	*method = g_strdup(first[0]);
	*path = g_strdup(first[1]);
	g_strfreev(first);

	for (i = 1; lines[i] != NULL; i++) {
		gchar *colon = strchr(lines[i], ':');
		gchar *key, *value;

		if (colon == NULL)
			continue;
		key = g_ascii_strdown(lines[i], colon - lines[i]);
		value = g_strdup(colon + 1);
		g_strstrip(value);
		g_hash_table_replace(headers, g_strstrip(key), value);
	}

	g_strfreev(lines);
	return TRUE;
}

static gboolean origin_is_local(const gchar *origin)
{
	if (origin == NULL || *origin == '\0')
		return TRUE;
	return g_str_has_prefix(origin, "http://localhost") ||
	       g_str_has_prefix(origin, "http://127.0.0.1") ||
	       g_str_has_prefix(origin, "http://[::1]") ||
	       g_ascii_strcasecmp(origin, "null") == 0;
}

static gboolean on_run(GThreadedSocketService *service, GSocketConnection *conn,
		       GObject *source, gpointer user_data)
{
	McpHttpServer *srv = user_data;
	GInputStream *in = g_io_stream_get_input_stream(G_IO_STREAM(conn));
	GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(conn));
	GByteArray *buf = g_byte_array_new();
	GHashTable *headers = NULL;
	gchar *method = NULL, *path = NULL, *head = NULL;
	gchar *body = NULL;
	gsize header_end = 0, content_length = 0;
	guint8 chunk[4096];
	McpHttpRequest req;
	McpHttpResponse res;
	gssize n;
	gsize i;
	gint64 deadline;
	gboolean have_head = FALSE;

	(void)service;
	(void)source;

	g_socket_set_timeout(g_socket_connection_get_socket(conn), MCP_HTTP_TIMEOUT);
	deadline = g_get_monotonic_time() + ((gint64)MCP_HTTP_TIMEOUT * G_TIME_SPAN_SECOND);

	while (!have_head) {
		if (g_get_monotonic_time() > deadline) {
			send_error(out, 408, "request timeout");
			goto done;
		}
		n = g_input_stream_read(in, chunk, sizeof chunk, NULL, NULL);
		if (n <= 0)
			goto done;
		g_byte_array_append(buf, chunk, (guint)n);
		if (buf->len > MCP_HTTP_MAX_HEADER) {
			send_error(out, 413, "header too large");
			goto done;
		}
		for (i = 3; i < buf->len; i++) {
			if (buf->data[i - 3] == '\r' && buf->data[i - 2] == '\n' &&
			    buf->data[i - 1] == '\r' && buf->data[i] == '\n') {
				header_end = i + 1;
				have_head = TRUE;
				break;
			}
		}
	}

	head = g_strndup((const gchar *)buf->data, header_end - 4);
	headers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	if (!parse_head(head, &method, &path, headers)) {
		send_error(out, 400, "malformed request");
		goto done;
	}

	{
		const gchar *cl = header_lookup(headers, "content-length");
		if (cl != NULL)
			content_length = (gsize)g_ascii_strtoull(cl, NULL, 10);
	}
	if (content_length > MCP_HTTP_MAX_BODY) {
		send_error(out, 413, "body too large");
		goto done;
	}

	while (buf->len - header_end < content_length) {
		if (g_get_monotonic_time() > deadline) {
			send_error(out, 408, "request timeout");
			goto done;
		}
		n = g_input_stream_read(in, chunk, sizeof chunk, NULL, NULL);
		if (n <= 0)
			break;
		g_byte_array_append(buf, chunk, (guint)n);
	}
	if (buf->len - header_end < content_length) {
		send_error(out, 400, "incomplete body");
		goto done;
	}
	body = g_strndup((const gchar *)buf->data + header_end, content_length);

	if (!origin_is_local(header_lookup(headers, "origin"))) {
		send_error(out, 403, "cross-origin request rejected");
		goto done;
	}

	memset(&req, 0, sizeof req);
	req.method = method;
	req.path = path;
	req.authorization = header_lookup(headers, "authorization");
	req.origin = header_lookup(headers, "origin");
	req.content_type = header_lookup(headers, "content-type");
	req.accept = header_lookup(headers, "accept");
	req.session_id = header_lookup(headers, "mcp-session-id");
	req.body = body;
	req.body_len = content_length;

	memset(&res, 0, sizeof res);
	res.status = 200;

	srv->handler(&req, &res, srv->user_data);

	send_response(out, res.status,
		      res.content_type != NULL ? res.content_type : "application/json",
		      res.extra_headers, res.body);

	g_free(res.content_type);
	g_free(res.body);
	g_free(res.extra_headers);

done:
	g_free(head);
	g_free(method);
	g_free(path);
	g_free(body);
	if (headers != NULL)
		g_hash_table_destroy(headers);
	g_byte_array_free(buf, TRUE);
	g_io_stream_close(G_IO_STREAM(conn), NULL, NULL);
	return TRUE;
}

McpHttpServer *mcp_http_server_start(const gchar *bind_address, guint16 port,
				     McpHttpHandler handler, gpointer user_data,
				     GError **error)
{
	McpHttpServer *srv;
	GInetAddress *inet;
	GSocketAddress *addr;
	GSocketAddress *effective = NULL;

	g_return_val_if_fail(handler != NULL, NULL);

	inet = g_inet_address_new_from_string(bind_address != NULL ? bind_address : "127.0.0.1");
	if (inet == NULL)
		inet = g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4);
	addr = g_inet_socket_address_new(inet, port);

	srv = g_new0(McpHttpServer, 1);
	srv->handler = handler;
	srv->user_data = user_data;
	srv->service = g_threaded_socket_service_new(4);

	if (!g_socket_listener_add_address(G_SOCKET_LISTENER(srv->service), addr,
					   G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_TCP,
					   NULL, &effective, error)) {
		g_object_unref(srv->service);
		g_free(srv);
		g_object_unref(addr);
		g_object_unref(inet);
		return NULL;
	}

	srv->port = port;
	if (effective != NULL) {
		srv->port = g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(effective));
		g_object_unref(effective);
	}

	g_signal_connect(srv->service, "run", G_CALLBACK(on_run), srv);
	g_socket_service_start(srv->service);

	g_object_unref(addr);
	g_object_unref(inet);
	return srv;
}

void mcp_http_server_stop(McpHttpServer *server)
{
	if (server == NULL)
		return;
	g_socket_service_stop(server->service);
	g_socket_listener_close(G_SOCKET_LISTENER(server->service));
	g_object_unref(server->service);
	g_free(server);
}

guint16 mcp_http_server_port(McpHttpServer *server)
{
	return server != NULL ? server->port : 0;
}
