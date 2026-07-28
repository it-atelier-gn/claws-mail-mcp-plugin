/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Georg Nelles */

#ifndef MCP_PLUGIN_H
#define MCP_PLUGIN_H

#include <glib.h>

#define MCP_PLUGIN_NAME    "MCP Server"
#define MCP_PLUGIN_VERSION "0.1.0"
#define MCP_PROTOCOL_VERSION "2025-06-18"

typedef struct {
	gchar   *bind_address;
	guint16  port;
	gchar   *token;
	gboolean read_only;
} McpConfig;

const McpConfig *mcp_plugin_config(void);
gboolean         mcp_plugin_read_only(void);

#endif
