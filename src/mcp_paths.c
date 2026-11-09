/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Georg Nelles */

#include <string.h>

#include "mcp_paths.h"
#include "mcp_json.h"

#define MCP_MAX_NAME_BYTES 180
#define MCP_MAX_EXT_BYTES  16

static gboolean is_sep(gchar c)
{
	return c == '/' || c == '\\';
}

static gboolean has_parent_ref(const gchar *path)
{
	const gchar *p = path;

	while (*p != '\0') {
		const gchar *start = p;

		while (*p != '\0' && !is_sep(*p))
			p++;
		if (p - start == 2 && start[0] == '.' && start[1] == '.')
			return TRUE;
		while (is_sep(*p))
			p++;
	}
	return FALSE;
}

static gboolean is_reserved_device(const gchar *name, gsize len)
{
	static const gchar *const plain[] = { "CON", "PRN", "AUX", "NUL" };
	gsize i;

	if (len == 3) {
		for (i = 0; i < G_N_ELEMENTS(plain); i++) {
			if (g_ascii_strncasecmp(name, plain[i], 3) == 0)
				return TRUE;
		}
	}
	if (len == 4 && (g_ascii_strncasecmp(name, "COM", 3) == 0 ||
			 g_ascii_strncasecmp(name, "LPT", 3) == 0) &&
	    name[3] >= '1' && name[3] <= '9')
		return TRUE;
	return FALSE;
}

static gboolean component_is_reserved(const gchar *name)
{
	gsize stem = strcspn(name, ".");

	while (stem > 0 && name[stem - 1] == ' ')
		stem--;
	return is_reserved_device(name, stem);
}

static gboolean prefix_matches(const gchar *full, const gchar *root, gsize rlen)
{
#ifdef G_OS_WIN32
	return g_ascii_strncasecmp(full, root, rlen) == 0;
#else
	return strncmp(full, root, rlen) == 0;
#endif
}

static gboolean inside_root(const gchar *full, const gchar *root, gsize *rest)
{
	gsize rlen = strlen(root);

	if (rlen == 0 || !prefix_matches(full, root, rlen))
		return FALSE;
	if (is_sep(root[rlen - 1])) {
		*rest = rlen;
		return full[rlen] != '\0';
	}
	if (!is_sep(full[rlen]) || full[rlen + 1] == '\0')
		return FALSE;
	*rest = rlen + 1;
	return TRUE;
}

static gboolean rest_is_safe(const gchar *full, gsize rest)
{
	gchar *probe = g_strdup(full);
	gsize i = rest;
	gboolean ok = TRUE;

	for (;;) {
		gsize start = i;
		gchar saved;

		while (probe[i] != '\0' && !is_sep(probe[i]))
			i++;
		saved = probe[i];
		probe[i] = '\0';
#ifdef G_OS_WIN32
		if (strchr(probe + start, ':') != NULL || component_is_reserved(probe + start))
			ok = FALSE;
#endif
		if (ok && g_file_test(probe, G_FILE_TEST_IS_SYMLINK))
			ok = FALSE;
		probe[i] = saved;
		if (!ok || saved == '\0')
			break;
		i++;
	}
	g_free(probe);
	return ok;
}

gchar *mcp_path_resolve(const gchar *root, const gchar *requested)
{
	gchar *root_c, *full;
	gsize rest = 0;

	if (root == NULL || *root == '\0' || requested == NULL || *requested == '\0')
		return NULL;
	if (has_parent_ref(requested))
		return NULL;

	root_c = g_canonicalize_filename(root, NULL);
	full = g_canonicalize_filename(requested, root_c);

	if (!inside_root(full, root_c, &rest) || !rest_is_safe(full, rest)) {
		g_free(full);
		full = NULL;
	}
	g_free(root_c);
	return full;
}

gchar *mcp_safe_filename(const gchar *raw)
{
	const gchar *base, *p;
	gchar *valid, *name;
	GString *out;
	gsize start, end;

	if (raw == NULL)
		return g_strdup("attachment");

	base = raw;
	for (p = raw; *p != '\0'; p++) {
		if (is_sep(*p))
			base = p + 1;
	}

	valid = g_utf8_make_valid(base, -1);
	out = g_string_new(NULL);
	for (p = valid; *p != '\0'; p = g_utf8_next_char(p)) {
		gunichar c = g_utf8_get_char(p);

		if (c < 0x20 || c == 0x7f || (c < 0x80 && strchr("<>:\"/\\|?*", (gchar)c) != NULL))
			g_string_append_c(out, '_');
		else
			g_string_append_unichar(out, c);
	}
	g_free(valid);

	start = 0;
	while (start < out->len && (out->str[start] == '.' || out->str[start] == ' '))
		start++;
	end = out->len;
	while (end > start && (out->str[end - 1] == '.' || out->str[end - 1] == ' '))
		end--;
	name = g_strndup(out->str + start, end - start);
	g_string_free(out, TRUE);

	if (*name == '\0') {
		g_free(name);
		return g_strdup("attachment");
	}

	if (strlen(name) > MCP_MAX_NAME_BYTES) {
		const gchar *dot = strrchr(name, '.');
		gsize ext_len = dot != NULL ? strlen(dot) : 0;
		gchar *cut;

		if (ext_len == 0 || ext_len > MCP_MAX_EXT_BYTES)
			ext_len = 0;
		cut = g_strndup(name, mcp_utf8_boundary(name, MCP_MAX_NAME_BYTES - ext_len));
		if (ext_len > 0) {
			gchar *joined = g_strconcat(cut, dot, NULL);
			g_free(cut);
			cut = joined;
		}
		g_free(name);
		name = cut;
	}

	if (component_is_reserved(name)) {
		gchar *prefixed = g_strconcat("_", name, NULL);
		g_free(name);
		name = prefixed;
	}
	return name;
}

gchar *mcp_unique_path(const gchar *dir, const gchar *name)
{
	gchar *path = g_build_filename(dir, name, NULL);
	const gchar *dot;
	gchar *stem;
	const gchar *ext;
	gint i;

	if (!g_file_test(path, G_FILE_TEST_EXISTS))
		return path;
	g_free(path);

	dot = strrchr(name, '.');
	if (dot != NULL && dot != name) {
		stem = g_strndup(name, (gsize)(dot - name));
		ext = dot;
	} else {
		stem = g_strdup(name);
		ext = "";
	}

	for (i = 1; i < 10000; i++) {
		gchar *candidate = g_strdup_printf("%s-%d%s", stem, i, ext);

		path = g_build_filename(dir, candidate, NULL);
		g_free(candidate);
		if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
			g_free(stem);
			return path;
		}
		g_free(path);
	}
	g_free(stem);
	return NULL;
}
