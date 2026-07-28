#!/usr/bin/env bash
# Builds a minimal Claws Mail from source and installs only what a plugin needs:
# the public headers, claws-mail.pc, and the import library.
#
# Installs into $PREFIX (default: ./.claws-sdk). The plugin build then uses
#   PKG_CONFIG_PATH=$PREFIX/lib/pkgconfig  CLAWS_LIBDIR=$PREFIX/lib
#
# configure ignores unknown --disable-*-plugin options, so the plugin list may
# be a superset. libetpan is disabled to avoid the IMAP/NNTP dependency chain;
# the plugin only uses the folder API, which does not require it.
set -euo pipefail

CLAWS_VERSION="${CLAWS_VERSION:-4.3.1}"
PREFIX="${PREFIX:-$PWD/.claws-sdk}"
JOBS="$(nproc 2>/dev/null || echo 2)"

plugins="acpi_notifier address_keeper archive att_remover attachwarner \
  bogofilter bsfilter clamd fancy fetchinfo gdata geolocation libravatar \
  litehtml_viewer mailmbox managesieve newmail notification pdf_viewer perl \
  pgpcore pgpinline pgpmime python rssyl smime spam_report spamassassin \
  tnef_parse vcalendar"
disable=""
for p in $plugins; do disable="$disable --disable-${p}-plugin"; done

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cd "$work"

url="https://www.claws-mail.org/download.php?file=releases/claws-mail-${CLAWS_VERSION}.tar.xz"
curl -fSL "$url" -o claws.tar.xz
tar xf claws.tar.xz
cd "claws-mail-${CLAWS_VERSION}"

export CFLAGS="-D_POSIX_THREAD_SAFE_FUNCTIONS ${CFLAGS:-}"
# shellcheck disable=SC2086
./configure --prefix="$PREFIX" --disable-libetpan --disable-manual $disable
make -j"$JOBS"
make install

# make install does not ship the import library; place it where the plugin
# Makefile expects it (-lclaws-mail).
if [ -f src/libclaws.a ]; then
	cp -f src/libclaws.a "$PREFIX/lib/libclaws-mail.dll.a"
fi

PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" pkg-config --exists claws-mail \
	&& echo "claws-mail SDK ready in $PREFIX ($(PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" pkg-config --modversion claws-mail))"
