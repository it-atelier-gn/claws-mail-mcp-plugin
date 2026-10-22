# claws-mail-mcp-plugin

[![CI](https://github.com/it-atelier-gn/claws-mail-mcp-plugin/actions/workflows/ci.yml/badge.svg)](https://github.com/it-atelier-gn/claws-mail-mcp-plugin/actions/workflows/ci.yml)
[![Claws Mail](https://img.shields.io/badge/Claws%20Mail-supported%20versions-blue)](ci/claws-versions.json)
[![License: GPL v3+](https://img.shields.io/badge/License-GPLv3%2B-blue.svg)](LICENSE)

Native Claws Mail plugin that exposes the running mail client as an MCP server.

It runs inside Claws Mail and serves a local, loopback-only HTTP endpoint (JSON-RPC over MCP, bearer-token protected). Any MCP-capable AI tool can then read folders and messages, search, compose, reply, forward, manage folders and flags, and send or receive mail through your already-configured accounts. Endpoint, port, token and read-only mode are set in `mcp_pluginrc` inside the Claws Mail config directory; the plugin writes a ready-to-paste client config to `mcp_client.json` in the same place on every start.

---

## Installing the plugin

Prebuilt binaries are attached to each [release](https://github.com/it-atelier-gn/claws-mail-mcp-plugin/releases), one per OS and supported Claws Mail version (e.g. `mcp-linux-claws4.3.1.so`, `mcp-windows-claws4.3.0.dll`). Pick the file matching your Claws Mail version and OS, then:

1. Save it in a folder of your choice, for example `~/.claws-mail/plugins` or `%APPDATA%\Claws-mail\plugins` on Windows, as `mcp.so` or `mcp.dll`. Any file name works except one ending in `_plugin.so` or `_plugin.dll`: Claws Mail does not load such a file on startup and drops it from the plugin list.
2. In Claws Mail, open Configuration > Plugins, click Load... and select the file.

Claws Mail loads the plugin again on every start.

To build from source instead, see [Building](#building).

---

## Connecting an AI tool

Once loaded, the plugin writes the real endpoint URL and bearer token to `mcp_client.json` in the Claws Mail config directory. Use those two values with whichever method below matches your tool. The server is loopback-only (`127.0.0.1`), so it is reachable only from AI tools running on the same machine as Claws Mail.

### Claude Code

```
claude mcp add --transport http claws-mail http://127.0.0.1:8765/mcp \
  --header "Authorization: Bearer <TOKEN>"
```

### Claude Desktop

Settings > Developer > Edit Config, then merge in `packaging/claude/mcp.json`
(fill in `<TOKEN>`). Do not use the "Add custom connector" UI for this server:
that flow connects from Anthropic's servers, not from your machine, so it
cannot reach a loopback address.

### VS Code / GitHub Copilot Chat

```
code --add-mcp "{\"name\":\"claws-mail\",\"type\":\"http\",\"url\":\"http://127.0.0.1:8765/mcp\",\"headers\":{\"Authorization\":\"Bearer <TOKEN>\"}}"
```

Or copy `packaging/vscode/mcp.json` to `.vscode/mcp.json`; it prompts for the
token instead of storing it in the file.

### Codex CLI

```
export CLAWS_MCP_TOKEN=<TOKEN>
codex mcp add claws-mail --url http://127.0.0.1:8765/mcp --bearer-token-env-var CLAWS_MCP_TOKEN
```

This matches `packaging/codex/config.toml`, which Codex reads from
`~/.codex/config.toml`.

### MCP registry

The plugin is listed in the [MCP registry](https://registry.modelcontextprotocol.io) as `io.github.it-atelier-gn/claws-mail-mcp-plugin`. The listing carries no connection details; install the plugin and connect your tool as described above.

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

The plugin is built at `build/mcp.so`.

### Windows (MSYS2)

Install the MINGW64 toolchain and dependencies:

```
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-meson mingw-w64-x86_64-ninja \
  mingw-w64-x86_64-pkgconf mingw-w64-x86_64-gtk3 mingw-w64-x86_64-glib2 \
  mingw-w64-x86_64-gnutls mingw-w64-x86_64-libgcrypt mingw-w64-x86_64-enchant
```

Build the Claws Mail SDK and set `PKG_CONFIG_PATH` as above, then run the same
`meson setup` / `meson compile` / `meson test` commands. The plugin is built at
`build/mcp.dll`.

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
`pkg-config --variable=plugindir claws-mail`. Load it from there once under
Configuration > Plugins > Load...

---

## Supported Claws Mail versions

Each release contains a Linux and a Windows build for every Claws Mail
version listed in [`ci/claws-versions.json`](ci/claws-versions.json).

---

## License

GPL-3.0-or-later, see [LICENSE](LICENSE).
