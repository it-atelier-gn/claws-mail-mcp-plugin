/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Georg Nelles */

#ifndef MCP_PLUGIN_H
#define MCP_PLUGIN_H

#include <glib.h>

#define MCP_PLUGIN_NAME    "MCP Server"
#ifndef MCP_PLUGIN_VERSION
#define MCP_PLUGIN_VERSION "0.0.0-dev"
#endif
#define MCP_PROTOCOL_VERSION "2025-06-18"

typedef struct {
	gchar   *bind_address;
	guint16  port;
	gchar   *token;
	gboolean read_only;
	gchar   *file_dir;
} McpConfig;

const McpConfig *mcp_plugin_config(void);
gboolean         mcp_plugin_read_only(void);
const gchar     *mcp_plugin_file_dir(void);

#endif
