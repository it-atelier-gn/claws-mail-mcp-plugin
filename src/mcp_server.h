/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Georg Nelles */

#ifndef MCP_SERVER_H
#define MCP_SERVER_H

#include <glib.h>

gboolean mcp_server_start(gchar **error);
void     mcp_server_stop(void);
guint16  mcp_server_port(void);
gchar   *mcp_server_endpoint(void);

gchar *mcp_server_handle_message(const gchar *body, gsize len, gboolean *has_response);

#endif
