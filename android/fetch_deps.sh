#!/bin/sh
# fetch_deps.sh: fetch the pinned third-party sources (libusb) that
# app/src/main/cpp/CMakeLists.txt compiles. Idempotent: an existing checkout at
# the pinned commit is kept, anything else is replaced. Sources are compiled only
# by our CMake; their own build scripts are never run.
set -eu
mkdir -p "$(dirname "$0")/app/src/main/cpp/third_party"
cd "$(dirname "$0")/app/src/main/cpp"

fetch() { # name url tag commit
	d="third_party/$1"
	if [ -d "$d/.git" ] && [ "$(git -C "$d" rev-parse HEAD)" = "$4" ]; then
		echo "$1 $3: up to date"
		return
	fi
	rm -rf "$d"
	git -c advice.detachedHead=false clone -q --depth 1 --branch "$3" "$2" "$d"
	got=$(git -C "$d" rev-parse HEAD)
	if [ "$got" != "$4" ]; then
		echo "$1 $3: got commit $got, expected $4" >&2
		rm -rf "$d"
		exit 1
	fi
	echo "$1 $3: fetched"
}

fetch libusb https://github.com/libusb/libusb.git v1.0.29 15a7ebb4d426c5ce196684347d2b7cafad862626
