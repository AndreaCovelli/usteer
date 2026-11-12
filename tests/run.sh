#!/bin/sh
#
# Build and run the host tests: once as a plain build and once with
# AddressSanitizer, UndefinedBehaviorSanitizer and LeakSanitizer.
#
# The tests need libubox built for the host (headers and static library) and
# the libubus header. In an OpenWrt SDK these are staging_dir/host and, once
# the ubus package is compiled, staging_dir/target-*/usr/include.

set -eu

die() {
	echo "error: $*" >&2
	exit 1
}

if [ "$#" -ne 2 ]; then
	echo "usage: $0 LIBUBOX_PREFIX UBUS_INCLUDE_DIR" >&2
	exit 2
fi

tests_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
libubox_prefix=$1
ubus_include=$2
cc=${CC:-cc}

[ -f "$libubox_prefix/include/libubox/blobmsg.h" ] ||
	die "libubox headers not found in $libubox_prefix/include"
[ -f "$libubox_prefix/lib/libubox.a" ] ||
	die "libubox.a not found in $libubox_prefix/lib"
[ -f "$ubus_include/libubus.h" ] ||
	die "libubus.h not found in $ubus_include"

out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

# The ubus directory may hold headers of another architecture: search it
# last, so that it only provides what the host does not have.
build() {
	name=$1
	shift
	"$cc" -std=gnu99 -O1 -g -Wall -Werror -Wno-unused-function "$@" \
		-I"$libubox_prefix/include" -idirafter "$ubus_include" \
		"$tests_dir/test-policy.c" "$libubox_prefix/lib/libubox.a" \
		-o "$out/$name"
}

echo "== plain build"
build plain
"$out/plain"

echo "== sanitizer build"
build sanitized -fsanitize=address,undefined -fno-sanitize-recover=undefined \
	-fno-omit-frame-pointer
ASAN_OPTIONS=detect_leaks=1 "$out/sanitized"

echo "all tests passed"
