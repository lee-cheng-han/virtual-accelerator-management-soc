#!/bin/sh

set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$repo_root/tools/versions.env"

source_dir=${QEMU_SRC:-"$repo_root/build/qemu-src"}
build_dir=${QEMU_BUILD_DIR:-"$source_dir/build-vams"}
jobs=${VAMS_BUILD_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
marker="$build_dir/.vams-patch-series"
generated_files='hw/misc/vams_mgmt.c hw/misc/vams_pcie.c hw/riscv/vams_riscv.c include/hw/misc/vams_abi.h include/hw/misc/vams_mgmt.h include/hw/misc/vams_pcie.h'

source_hash()
{
	(
		cd "$source_dir"
		git diff --binary
		sha256sum $generated_files
	) | sha256sum | awk '{print $1}'
}

mkdir -p "$(dirname -- "$source_dir")"
if [ ! -d "$source_dir/.git" ]; then
	mkdir -p "$source_dir"
	git -C "$source_dir" init --quiet
	git -C "$source_dir" remote add origin \
		https://gitlab.com/qemu-project/qemu.git
	git -C "$source_dir" fetch --depth 1 origin "$VAMS_QEMU_COMMIT"
	git -C "$source_dir" checkout --quiet --detach FETCH_HEAD
fi

actual_commit=$(git -C "$source_dir" rev-parse HEAD)
if [ "$actual_commit" != "$VAMS_QEMU_COMMIT" ]; then
	echo "unexpected QEMU revision: $actual_commit" >&2
	echo "expected: $VAMS_QEMU_COMMIT" >&2
	exit 1
fi

actual_patch_hash=$(
	cd "$repo_root"
	LC_ALL=C sha256sum qemu/patches/*.patch | sha256sum | awk '{print $1}'
)
if [ "$actual_patch_hash" != "$VAMS_QEMU_PATCH_SHA256" ]; then
	echo "QEMU patch-series hash mismatch: $actual_patch_hash" >&2
	exit 1
fi

if [ -f "$marker" ]; then
	marker_value=$(sed -n '1p' "$marker")
	if [ "$marker_value" != "$VAMS_QEMU_COMMIT $VAMS_QEMU_PATCH_SHA256" ]; then
		echo 'QEMU build marker does not match pinned inputs' >&2
		exit 1
	fi
else
	if git -C "$source_dir" diff --quiet &&
	   git -C "$source_dir" diff --cached --quiet; then
		for patch in "$repo_root"/qemu/patches/*.patch; do
			git -C "$source_dir" apply "$patch"
		done
	elif [ "$(source_hash)" != "$VAMS_QEMU_SOURCE_SHA256" ]; then
		echo 'QEMU source has unrecognized changes and no VAMS build marker' >&2
		exit 1
	fi
	mkdir -p "$build_dir"
	printf '%s %s\n' "$VAMS_QEMU_COMMIT" "$VAMS_QEMU_PATCH_SHA256" >"$marker"
fi

if [ "$(source_hash)" != "$VAMS_QEMU_SOURCE_SHA256" ]; then
	echo 'patched QEMU source does not match the pinned source hash' >&2
	exit 1
fi

(
	cd "$build_dir"
	../configure \
		--target-list=riscv32-softmmu,x86_64-softmmu \
		--disable-guest-agent --disable-tools --disable-vnc \
		--disable-docs --enable-plugins
)

ninja -C "$build_dir" -j "$jobs" \
	qemu-system-riscv32 qemu-system-x86_64

for executable in qemu-system-riscv32 qemu-system-x86_64; do
	test -x "$build_dir/$executable" || {
		echo "missing QEMU executable: $build_dir/$executable" >&2
		exit 1
	}
done

version=$($build_dir/qemu-system-x86_64 --version | sed -n '1p')
commit_prefix=$(printf '%.10s' "$VAMS_QEMU_COMMIT")
case "$version" in
	*"$VAMS_QEMU_VERSION"*"$commit_prefix"*) ;;
	*) echo "unexpected built QEMU version: $version" >&2; exit 1 ;;
esac

echo "VAMS QEMU preparation: PASS ($VAMS_QEMU_VERSION $VAMS_QEMU_COMMIT)"
