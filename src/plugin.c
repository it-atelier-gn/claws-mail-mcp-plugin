/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Georg Nelles */

#include "claws-features.h"

#include <glib.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>

#include "common/version.h"
#include "common/utils.h"
#include "plugin.h"

#include "mcp_plugin.h"
#include "mcp_server.h"

static void write_client_snippet(void)
{
	const McpConfig *cfg = mcp_plugin_config();
	gchar *path = g_strconcat(get_rc_dir(), G_DIR_SEPARATOR_S, "mcp_client.json", NULL);
	gchar *endpoint = mcp_server_endpoint();
	gchar *text;

	text = g_strdup_printf(
		"{\n"
		"  \"mcpServers\": {\n"
		"    \"claws-mail\": {\n"
		"      \"type\": \"http\",\n"
		"      \"url\": \"%s\",\n"
		"      \"headers\": { \"Authorization\": \"Bearer %s\" }\n"
		"    }\n"
		"  }\n"
		"}\n",
		endpoint, cfg->token != NULL ? cfg->token : "");

	if (g_file_set_contents(path, text, -1, NULL))
		g_chmod(path, 0600);

	g_free(text);
	g_free(endpoint);
	g_free(path);
}

gint plugin_init(gchar **error)
{
	gchar *start_error = NULL;
	gchar *endpoint;

	if (!check_plugin_version(MAKE_NUMERIC_VERSION(3, 9, 0, 0), VERSION_NUMERIC,
				  MCP_PLUGIN_NAME, error))
		return -1;

	if (!mcp_server_start(&start_error)) {
		*error = start_error != NULL ? start_error
					     : g_strdup("could not start the MCP server");
		return -1;
	}

	write_client_snippet();

	endpoint = mcp_server_endpoint();
	g_message("MCP server listening on %s (config: %s%cmcp_pluginrc)",
		  endpoint, get_rc_dir(), G_DIR_SEPARATOR);
	g_free(endpoint);

	return 0;
}

gboolean plugin_done(void)
{
	mcp_server_stop();
	return TRUE;
}

const gchar *plugin_name(void)
{
	return _(MCP_PLUGIN_NAME);
}

const gchar *plugin_desc(void)
{
	return _("Serves the running Claws Mail instance to MCP clients over a local "
		 "HTTP endpoint (JSON-RPC, bearer-token protected, loopback only).\n\n"
		 "Tools: list_folders, list_messages, read_message, get_source, "
		 "search_messages, compose, reply, forward, receive_all, send_queued, "
		 "set_read, set_flag, mark_all_read, move_message, copy_message, "
		 "delete_message, empty_trash, export_message, import_message, "
		 "create_folder, rename_folder, delete_folder, select_folder, "
		 "list_accounts, status.\n\n"
		 "Endpoint, port, token and read_only mode are configured in "
		 "mcp_pluginrc inside the Claws Mail configuration directory; a ready to "
		 "paste client configuration is written to mcp_client.json.");
}

const gchar *plugin_type(void)
{
	return "GTK3";
}

const gchar *plugin_licence(void)
{
	return "GPL3+";
}

const gchar *plugin_version(void)
{
	return MCP_PLUGIN_VERSION;
}

struct PluginFeature *plugin_provides(void)
{
	static struct PluginFeature features[] = {
		{ PLUGIN_UTILITY, N_("MCP server") },
		{ PLUGIN_NOTHING, NULL }
	};

	return features;
}
