# claws-mail-mcp-plugin

[![CI](https://github.com/it-atelier-gn/claws-mail-mcp-plugin/actions/workflows/ci.yml/badge.svg)](https://github.com/it-atelier-gn/claws-mail-mcp-plugin/actions/workflows/ci.yml)
![Claws Mail](https://img.shields.io/badge/Claws%20Mail-4.3.0%20%7C%204.3.1-blue)
[![License: GPL v3+](https://img.shields.io/badge/License-GPLv3%2B-blue.svg)](LICENSE)

Native Claws Mail plugin that exposes the running mail client as an MCP server.

It runs inside Claws Mail and serves a local, loopback-only HTTP endpoint (JSON-RPC over MCP, bearer-token protected). Any MCP-capable AI tool can then read folders and messages, search, compose, reply, forward, manage folders and flags, and send or receive mail through your already-configured accounts. Endpoint, port, token and read-only mode are set in `mcp_pluginrc` inside the Claws Mail config directory; the plugin writes a ready-to-paste client config to `mcp_client.json` in the same place on every start.

---

## Installing the plugin

Prebuilt binaries are attached to each [release](https://github.com/it-atelier-gn/claws-mail-mcp-plugin/releases), one per OS and supported Claws Mail version (e.g. `mcp_plugin-linux-claws4.3.1.so`, `mcp_plugin-windows-claws4.3.0.dll`). Pick the file matching your Claws Mail version and OS, then:

1. Copy it into your Claws Mail plugin directory (`pkg-config --variable=plugindir claws-mail`, typically `~/.claws-mail/plugins` or `%APPDATA%\Claws-mail\plugins` on Windows).
2. Restart Claws Mail.
3. Enable it under Configuration > Plugins if it is not loaded automatically.

To build from source instead, see [Building](#building).

---

## Connecting an AI tool

Once loaded, the plugin writes a ready-to-use connector config to `mcp_client.json` in the Claws Mail config directory. Copy the values from there (endpoint URL and bearer token) into your tool of choice. Template files with the same shape are in `packaging/`:

- `packaging/claude/mcp.json`: Claude Desktop (Settings > Developer > Edit Config) and Claude Code (`.mcp.json`)
- `packaging/vscode/mcp.json`: VS Code / GitHub Copilot Chat (`.vscode/mcp.json`), prompts for the token instead of storing it in the file
- `packaging/codex/config.toml`: Codex CLI (`~/.codex/config.toml`), reads the token from the `CLAWS_MCP_TOKEN` environment variable

`packaging/registry/server.json` is a manifest for the [MCP registry](https://github.com/modelcontextprotocol/registry), ready to submit.

---

## Building

Requires Meson, Ninja, a C compiler, and the Claws Mail development files
(GTK3, GLib, GnuTLS, libgcrypt, enchant).

### Linux

Install dependencies (Debian/Ubuntu example):

```
sudo apt-get install build-essential meson ninja-build pkg-config \
  libgtk-3-dev libglib2.0-dev libgnutls28-dev libgcrypt20-dev libenchant-2-dev
```

If a `claws-mail` pkg-config file is not already on your system, build one with:

```
PREFIX="$PWD/.claws-sdk" ci/build-claws-sdk.sh
export PKG_CONFIG_PATH="$PWD/.claws-sdk/lib/pkgconfig"
```

Then configure and build:

```
meson setup build
meson compile -C build
meson test -C build
```

The plugin is built at `build/mcp_plugin.so`.

### Windows (MSYS2)

Install the MINGW64 toolchain and dependencies:

```
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-meson mingw-w64-x86_64-ninja \
  mingw-w64-x86_64-pkgconf mingw-w64-x86_64-gtk3 mingw-w64-x86_64-glib2 \
  mingw-w64-x86_64-gnutls mingw-w64-x86_64-libgcrypt mingw-w64-x86_64-enchant
```

Build the Claws Mail SDK and set `PKG_CONFIG_PATH` as above, then run the same
`meson setup` / `meson compile` / `meson test` commands. The plugin is built at
`build/mcp_plugin.dll`.

### Plugin version

The version compiled into the binary (returned by `plugin_version` and the
`status` tool) comes from `-Dplugin_version`, e.g.:

```
meson setup build -Dplugin_version=1.2.3
```

CI sets this from the pushed git tag on release builds. Without it, the build
falls back to `git describe`, then to `0.0.0-dev`.

### Installing

```
meson install -C build
```

This installs the plugin into the Claws Mail plugin directory reported by
`pkg-config --variable=plugindir claws-mail`.

---

## Supported Claws Mail versions

CI builds and tests the plugin against every version listed in
`.github/workflows/ci.yml` (currently 4.3.0 and 4.3.1), on both Linux and
Windows. Each combination gets its own release artifact.

---

## License

GPL-3.0-or-later, see [LICENSE](LICENSE).
