/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Georg Nelles */

#include "claws-features.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <glib/gstdio.h>

#include "common/utils.h"

#include "mcp_plugin.h"
#include "mcp_server.h"
#include "mcp_json.h"
#include "mcp_http.h"
#include "mcp_tools.h"

#define MCP_DEFAULT_PORT 8765
#define MCP_RC_FILE      "mcp_pluginrc"
#define MCP_CALL_TIMEOUT_SEC 120

static McpConfig     config;
static McpHttpServer *server;

const McpConfig *mcp_plugin_config(void)
{
	return &config;
}

gboolean mcp_plugin_read_only(void)
{
	return config.read_only;
}

static gchar *config_path(void)
{
	return g_strconcat(get_rc_dir(), G_DIR_SEPARATOR_S, MCP_RC_FILE, NULL);
}

#define MCP_TOKEN_BYTES 32

#ifdef G_OS_WIN32
static gboolean secure_random(guint8 *buf, gsize len)
{
	gsize i = 0;

	while (i < len) {
		unsigned int r;
		gsize n;

		if (rand_s(&r) != 0)
			return FALSE;
		n = MIN(sizeof r, len - i);
		memcpy(buf + i, &r, n);
		i += n;
	}
	return TRUE;
}
#else
static gboolean secure_random(guint8 *buf, gsize len)
{
	FILE *f = fopen("/dev/urandom", "rb");
	gboolean ok;

	if (f == NULL)
		return FALSE;
	ok = fread(buf, 1, len, f) == len;
	fclose(f);
	return ok;
}
#endif

static gchar *generate_token(void)
{
	guint8 raw[MCP_TOKEN_BYTES];
	GString *s;
	gsize i;

	if (!secure_random(raw, sizeof raw)) {
		for (i = 0; i < sizeof raw; i++)
			raw[i] = (guint8)g_random_int_range(0, 256);
	}
	s = g_string_sized_new(sizeof raw * 2);
	for (i = 0; i < sizeof raw; i++)
		g_string_append_printf(s, "%02x", raw[i]);
	return g_string_free(s, FALSE);
}

static void config_write(const gchar *path)
{
	gchar *text = g_strdup_printf(
		"bind_address=%s\n"
		"port=%u\n"
		"token=%s\n"
		"read_only=%d\n",
		config.bind_address, (guint)config.port, config.token,
		config.read_only ? 1 : 0);

	if (g_file_set_contents(path, text, -1, NULL))
		g_chmod(path, 0600);
	g_free(text);
}

static void config_load(void)
{
	gchar *path = config_path();
	gchar *text = NULL;
	gboolean dirty = FALSE;

	g_free(config.bind_address);
	g_free(config.token);
	memset(&config, 0, sizeof config);
	config.bind_address = g_strdup("127.0.0.1");
	config.port = MCP_DEFAULT_PORT;
	config.read_only = FALSE;

	if (g_file_get_contents(path, &text, NULL, NULL)) {
		gchar **lines = g_strsplit(text, "\n", -1);
		gint i;

		for (i = 0; lines[i] != NULL; i++) {
			gchar *eq = strchr(lines[i], '=');
			gchar *key, *value;

			if (eq == NULL || lines[i][0] == '#')
				continue;
			key = g_strndup(lines[i], (gsize)(eq - lines[i]));
			value = g_strdup(eq + 1);
			g_strstrip(key);
			g_strstrip(value);

			if (strcmp(key, "bind_address") == 0 && *value != '\0') {
				g_free(config.bind_address);
				config.bind_address = g_strdup(value);
			} else if (strcmp(key, "port") == 0) {
				config.port = (guint16)g_ascii_strtoull(value, NULL, 10);
			} else if (strcmp(key, "token") == 0 && *value != '\0') {
				g_free(config.token);
				config.token = g_strdup(value);
			} else if (strcmp(key, "read_only") == 0) {
				config.read_only = (*value == '1' || *value == 't' || *value == 'y');
			}
			g_free(key);
			g_free(value);
		}
		g_strfreev(lines);
		g_free(text);
	} else {
		dirty = TRUE;
	}

	if (config.token == NULL) {
		config.token = generate_token();
		dirty = TRUE;
	}
	if (config.port == 0)
		config.port = MCP_DEFAULT_PORT;

	if (dirty)
		config_write(path);
	g_free(path);
}

typedef struct {
	const McpTool *tool;
	McpJson       *args;
	gchar         *result;
	gchar         *error;
	GMutex         mutex;
	GCond          cond;
	gboolean       done;
	gint           refs;
} ToolCall;

static void tool_call_unref(ToolCall *call)
{
	if (!g_atomic_int_dec_and_test(&call->refs))
		return;
	mcp_json_free(call->args);
	g_free(call->result);
	g_free(call->error);
	g_mutex_clear(&call->mutex);
	g_cond_clear(&call->cond);
	g_free(call);
}

static gboolean tool_call_in_main(gpointer data)
{
	ToolCall *call = data;
	gchar *error = NULL;
	gchar *result;

	result = call->tool->run(call->args, &error);

	g_mutex_lock(&call->mutex);
	call->result = result;
	call->error = error;
	call->done = TRUE;
	g_cond_signal(&call->cond);
	g_mutex_unlock(&call->mutex);

	tool_call_unref(call);
	return G_SOURCE_REMOVE;
}

static gchar *run_tool(const McpTool *tool, const McpJson *args, gchar **error)
{
	ToolCall *call = g_new0(ToolCall, 1);
	gint64 deadline;
	gchar *result = NULL;

	g_mutex_init(&call->mutex);
	g_cond_init(&call->cond);
	call->tool = tool;
	call->args = mcp_json_copy(args);
	call->refs = 2;

	g_main_context_invoke(NULL, tool_call_in_main, call);

	deadline = g_get_monotonic_time() + (MCP_CALL_TIMEOUT_SEC * G_TIME_SPAN_SECOND);
	g_mutex_lock(&call->mutex);
	while (!call->done) {
		if (!g_cond_wait_until(&call->cond, &call->mutex, deadline))
			break;
	}
	if (call->done) {
		result = call->result;
		call->result = NULL;
		*error = call->error;
		call->error = NULL;
	} else {
		*error = g_strdup("timed out waiting for Claws Mail");
	}
	g_mutex_unlock(&call->mutex);

	tool_call_unref(call);
	return result;
}

static void append_id(GString *out, const McpJson *id)
{
	g_string_append(out, "\"id\":");
	if (mcp_json_is(id, MCP_JSON_STRING) || mcp_json_is(id, MCP_JSON_NUMBER))
		mcp_json_write(id, out);
	else
		g_string_append(out, "null");
}

static gchar *rpc_error(const McpJson *id, gint code, const gchar *message)
{
	GString *out = g_string_new("{\"jsonrpc\":\"2.0\",");

	append_id(out, id);
	g_string_append_printf(out, ",\"error\":{\"code\":%d,\"message\":", code);
	mcp_json_escape(out, message);
	g_string_append(out, "}}");
	return g_string_free(out, FALSE);
}

static gchar *rpc_result(const McpJson *id, const gchar *result_json)
{
	GString *out = g_string_new("{\"jsonrpc\":\"2.0\",");

	append_id(out, id);
	g_string_append(out, ",\"result\":");
	g_string_append(out, result_json);
	g_string_append_c(out, '}');
	return g_string_free(out, FALSE);
}

static gchar *build_initialize_result(const McpJson *params)
{
	const McpJson *pv = mcp_json_member(params, "protocolVersion");
	GString *out = g_string_new("{");

	g_string_append(out, "\"protocolVersion\":");
	if (mcp_json_is(pv, MCP_JSON_STRING))
		mcp_json_write(pv, out);
	else
		g_string_append(out, "\"" MCP_PROTOCOL_VERSION "\"");

	g_string_append(out, ",\"capabilities\":{\"tools\":{\"listChanged\":false}}");
	g_string_append(out, ",\"serverInfo\":{\"name\":\"claws-mail\",\"title\":\"Claws Mail\",\"version\":\""
			MCP_PLUGIN_VERSION "\"}");
	g_string_append(out, ",\"instructions\":\"Tools operate on the running Claws Mail instance. "
			"Folders are addressed by identifier (e.g. #mh/Mail/inbox) or by the aliases "
			"inbox, sent, drafts, queue, trash. Mail is never sent without an explicit "
			"send_queued call; compose only opens a window.\"");
	g_string_append_c(out, '}');
	return g_string_free(out, FALSE);
}

static gchar *build_tools_list(void)
{
	guint n = 0, i;
	const McpTool *all = mcp_tools_all(&n);
	GString *out = g_string_new("{\"tools\":[");

	for (i = 0; i < n; i++) {
		if (i > 0)
			g_string_append_c(out, ',');
		g_string_append_c(out, '{');
		g_string_append(out, "\"name\":");
		mcp_json_escape(out, all[i].name);
		g_string_append(out, ",\"description\":");
		mcp_json_escape(out, all[i].description);
		g_string_append(out, ",\"inputSchema\":");
		g_string_append(out, all[i].input_schema);
		if (!all[i].mutating)
			g_string_append(out, ",\"annotations\":{\"readOnlyHint\":true}");
		g_string_append_c(out, '}');
	}
	g_string_append(out, "]}");
	return g_string_free(out, FALSE);
}

static gchar *build_tool_result(const gchar *text, gboolean is_error)
{
	GString *out = g_string_new("{\"content\":[{\"type\":\"text\",\"text\":");

	mcp_json_escape(out, text != NULL ? text : "");
	g_string_append_printf(out, "}],\"isError\":%s}", is_error ? "true" : "false");
	return g_string_free(out, FALSE);
}

static gchar *handle_tools_call(const McpJson *id, const McpJson *params)
{
	const gchar *name = mcp_json_member_string(params, "name", NULL);
	const McpJson *args = mcp_json_member(params, "arguments");
	const McpTool *tool;
	gchar *error = NULL;
	gchar *text;
	gchar *result_json;
	gchar *response;

	if (name == NULL)
		return rpc_error(id, -32602, "missing tool name");

	tool = mcp_tools_find(name);
	if (tool == NULL)
		return rpc_error(id, -32602, "unknown tool");

	if (tool->mutating && config.read_only) {
		result_json = build_tool_result("refused: the MCP plugin runs in read_only mode", TRUE);
		response = rpc_result(id, result_json);
		g_free(result_json);
		return response;
	}

	text = run_tool(tool, args, &error);
	if (text == NULL) {
		result_json = build_tool_result(error != NULL ? error : "tool failed", TRUE);
		g_free(error);
	} else {
		result_json = build_tool_result(text, FALSE);
		g_free(text);
		g_free(error);
	}

	response = rpc_result(id, result_json);
	g_free(result_json);
	return response;
}

static gchar *handle_rpc(const McpJson *msg, gboolean *has_response)
{
	const gchar *method = mcp_json_member_string(msg, "method", NULL);
	const McpJson *id = mcp_json_member(msg, "id");
	const McpJson *params = mcp_json_member(msg, "params");
	gchar *result_json;
	gchar *response;

	*has_response = mcp_json_is(id, MCP_JSON_STRING) || mcp_json_is(id, MCP_JSON_NUMBER);

	if (method == NULL) {
		if (!*has_response)
			return NULL;
		return rpc_error(id, -32600, "invalid request");
	}

	if (g_str_has_prefix(method, "notifications/")) {
		*has_response = FALSE;
		return NULL;
	}
	if (!*has_response)
		return NULL;

	if (strcmp(method, "initialize") == 0) {
		result_json = build_initialize_result(params);
		response = rpc_result(id, result_json);
		g_free(result_json);
		return response;
	}
	if (strcmp(method, "ping") == 0)
		return rpc_result(id, "{}");
	if (strcmp(method, "tools/list") == 0) {
		result_json = build_tools_list();
		response = rpc_result(id, result_json);
		g_free(result_json);
		return response;
	}
	if (strcmp(method, "tools/call") == 0)
		return handle_tools_call(id, params);

	return rpc_error(id, -32601, "method not found");
}

gchar *mcp_server_handle_message(const gchar *body, gsize len, gboolean *has_response)
{
	GError *error = NULL;
	McpJson *msg;
	gchar *response;

	*has_response = TRUE;

	msg = mcp_json_parse(body, (gssize)len, &error);
	if (msg == NULL) {
		gchar *text = g_strdup_printf("parse error: %s",
					      error != NULL ? error->message : "invalid JSON");
		response = rpc_error(NULL, -32700, text);
		g_free(text);
		g_clear_error(&error);
		return response;
	}

	if (mcp_json_is(msg, MCP_JSON_ARRAY)) {
		GString *out = g_string_new("[");
		guint i;
		gboolean any = FALSE;

		for (i = 0; i < mcp_json_length(msg); i++) {
			gboolean sub_response = FALSE;
			gchar *part = handle_rpc(mcp_json_index(msg, i), &sub_response);

			if (part == NULL || !sub_response) {
				g_free(part);
				continue;
			}
			if (any)
				g_string_append_c(out, ',');
			g_string_append(out, part);
			g_free(part);
			any = TRUE;
		}
		g_string_append_c(out, ']');
		mcp_json_free(msg);

		*has_response = any;
		if (!any) {
			g_string_free(out, TRUE);
			return NULL;
		}
		return g_string_free(out, FALSE);
	}

	response = handle_rpc(msg, has_response);
	mcp_json_free(msg);
	return response;
}

static gboolean token_ok(const gchar *authorization)
{
	const gchar *given;
	gsize len_a, len_b, i;
	guchar diff = 0;

	if (config.token == NULL || *config.token == '\0')
		return TRUE;
	if (authorization == NULL || !g_str_has_prefix(authorization, "Bearer "))
		return FALSE;

	given = authorization + 7;
	len_a = strlen(given);
	len_b = strlen(config.token);
	if (len_a != len_b)
		return FALSE;
	for (i = 0; i < len_a; i++)
		diff |= (guchar)(given[i] ^ config.token[i]);
	return diff == 0;
}

static void http_handler(const McpHttpRequest *req, McpHttpResponse *res, gpointer user_data)
{
	gboolean has_response = FALSE;
	gchar *response;

	(void)user_data;

	if (strcmp(req->method, "OPTIONS") == 0) {
		res->status = 204;
		res->extra_headers = g_strdup("Allow: POST, OPTIONS\r\n");
		return;
	}
	if (strcmp(req->method, "GET") == 0 || strcmp(req->method, "DELETE") == 0) {
		res->status = 405;
		res->extra_headers = g_strdup("Allow: POST, OPTIONS\r\n");
		res->body = g_strdup("{\"error\":\"only POST is supported\"}");
		return;
	}
	if (strcmp(req->method, "POST") != 0) {
		res->status = 405;
		res->body = g_strdup("{\"error\":\"unsupported method\"}");
		return;
	}
	if (!token_ok(req->authorization)) {
		res->status = 401;
		res->extra_headers = g_strdup("WWW-Authenticate: Bearer\r\n");
		res->body = g_strdup("{\"error\":\"invalid or missing bearer token\"}");
		return;
	}
	if (req->body == NULL || req->body_len == 0) {
		res->status = 400;
		res->body = g_strdup("{\"error\":\"empty body\"}");
		return;
	}

	response = mcp_server_handle_message(req->body, req->body_len, &has_response);
	if (!has_response || response == NULL) {
		g_free(response);
		res->status = 202;
		return;
	}

	res->status = 200;
	res->content_type = g_strdup("application/json");
	res->body = response;
}

gboolean mcp_server_start(gchar **error)
{
	GError *gerror = NULL;

	if (server != NULL)
		return TRUE;

	config_load();

	server = mcp_http_server_start(config.bind_address, config.port,
				       http_handler, NULL, &gerror);
	if (server == NULL) {
		if (error != NULL)
			*error = g_strdup_printf("cannot listen on %s:%u: %s",
						 config.bind_address, (guint)config.port,
						 gerror != NULL ? gerror->message : "unknown error");
		g_clear_error(&gerror);
		return FALSE;
	}
	return TRUE;
}

void mcp_server_stop(void)
{
	if (server != NULL) {
		mcp_http_server_stop(server);
		server = NULL;
	}
	g_free(config.bind_address);
	g_free(config.token);
	memset(&config, 0, sizeof config);
}

guint16 mcp_server_port(void)
{
	return mcp_http_server_port(server);
}

gchar *mcp_server_endpoint(void)
{
	return g_strdup_printf("http://%s:%u/mcp",
			       config.bind_address != NULL ? config.bind_address : "127.0.0.1",
			       (guint)mcp_server_port());
}
