#!/usr/bin/env bash
set -euo pipefail

CLAWS_VERSION="${CLAWS_VERSION:-4.3.1}"
PREFIX="${PREFIX:-$PWD/.claws-sdk}"
JOBS="$(nproc 2>/dev/null || echo 2)"

plugins="acpi_notifier address_keeper archive att_remover attachwarner \
  bogofilter bsfilter clamd dillo fancy fetchinfo gdata geolocation libravatar \
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

export CFLAGS="-D_POSIX_THREAD_SAFE_FUNCTIONS \
  -Wno-error=implicit-function-declaration \
  -Wno-error=incompatible-pointer-types \
  -Wno-error=int-conversion \
  -Wno-error=implicit-int \
  ${CFLAGS:-}"
# shellcheck disable=SC2086
./configure --prefix="$PREFIX" --disable-libetpan --disable-manual $disable
make -j"$JOBS"
make install

if [ -f src/libclaws.a ]; then
	cp -f src/libclaws.a "$PREFIX/lib/libclaws-mail.dll.a"
fi

PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" pkg-config --exists claws-mail \
	&& echo "claws-mail SDK ready in $PREFIX ($(PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" pkg-config --modversion claws-mail))"
