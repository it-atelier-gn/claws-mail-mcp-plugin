/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Georg Nelles */

#ifndef MCP_TOOLS_H
#define MCP_TOOLS_H

#include <glib.h>

#include "mcp_json.h"

typedef gchar *(*McpToolRun)(const McpJson *args, gchar **error);

typedef struct {
	const gchar *name;
	const gchar *description;
	const gchar *input_schema;
	McpToolRun   run;
	gboolean     mutating;
} McpTool;

const McpTool *mcp_tools_all(guint *n_tools);
const McpTool *mcp_tools_find(const gchar *name);

#endif
