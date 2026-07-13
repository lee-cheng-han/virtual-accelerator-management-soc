#!/bin/sh

set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
workspace=${ZEPHYR_WORKSPACE:-"$repo_root/build/zephyrproject"}
zephyr_dir="$workspace/zephyr"
venv=${ZEPHYR_VENV:-"$repo_root/build/zephyr-venv"}
version=v4.4.0
commit=684c9e8f32e4373a21098559f748f06915f950c9

mkdir -p "$workspace"

if [ ! -d "$zephyr_dir/.git" ]; then
    git clone --depth 1 --branch "$version" \
        https://github.com/zephyrproject-rtos/zephyr.git "$zephyr_dir"
fi

actual_commit=$(git -C "$zephyr_dir" rev-parse HEAD)
if [ "$actual_commit" != "$commit" ]; then
    echo "unexpected Zephyr revision: $actual_commit" >&2
    echo "expected: $commit ($version)" >&2
    exit 1
fi

if [ ! -x "$venv/bin/python" ]; then
    python3 -m venv "$venv"
fi

"$venv/bin/pip" install \
	--disable-pip-version-check \
	--no-cache-dir \
    --requirement "$zephyr_dir/scripts/requirements-base.txt"

if [ ! -d "$workspace/.west" ]; then
    (cd "$workspace" && "$venv/bin/west" init -l zephyr)
fi

echo "Zephyr preparation: PASS ($version $commit)"
