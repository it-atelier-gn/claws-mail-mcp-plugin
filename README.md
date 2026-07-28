# claws-mail-mcp-plugin
Plugin for Claws Mail that provides an MCP server

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

### Installing

```
meson install -C build
```

This installs the plugin into the Claws Mail plugin directory reported by
`pkg-config --variable=plugindir claws-mail`.
