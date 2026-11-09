/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Georg Nelles */

#ifndef MCP_PATHS_H
#define MCP_PATHS_H

#include <glib.h>

gchar *mcp_path_resolve(const gchar *root, const gchar *requested);
gchar *mcp_safe_filename(const gchar *raw);
gchar *mcp_unique_path(const gchar *dir, const gchar *name);

#endif
