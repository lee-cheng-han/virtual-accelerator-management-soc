#!/bin/busybox sh

set -u

PATH=/bin
export PATH

/bin/busybox --install -s /bin
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev 2>/dev/null || true

finish()
{
	code="$1"
	if [ "$code" -eq 0 ]; then
		echo 'VAMS Linux PCI queue, NOP, MSI-X, and cleanup smoke test: PASS'
	else
		echo 'VAMS Linux PCI queue, NOP, MSI-X, or cleanup smoke test: FAIL'
	fi
	sync
	devmem 0xf4 32 "$code"
	poweroff -f
	exit "$code"
}

fail()
{
	echo "FAIL: $*"
	finish 1
}

vams_device=
for device_path in /sys/bus/pci/devices/*; do
	[ -r "$device_path/vendor" ] || continue
	[ "$(cat "$device_path/vendor")" = '0x1b36' ] || continue
	[ "$(cat "$device_path/device")" = '0x1100' ] || continue
	vams_device="$device_path"
	break
done

[ -n "$vams_device" ] || fail 'VAMS PCI function did not enumerate'
echo "VAMS test device: ${vams_device##*/}"

step=1
while [ "$step" -le 8 ]; do
	insmod /vams_pci.ko probe_fail_step="$step" ||
		fail "module registration failed at injected step $step"
	[ ! -L "$vams_device/driver" ] ||
		fail "device remained bound after injected step $step"
	rmmod vams_pci || fail "module unload failed after injected step $step"
	step=$((step + 1))
done

insmod /vams_pci.ko probe_irq_selftest=1 probe_nop_selftest=1 ||
	fail 'normal probe, MSI-X, or NOP round-trip self-test failed'
[ -L "$vams_device/driver" ] || fail 'device did not bind'
[ "$(basename "$(readlink "$vams_device/driver")")" = 'vams_pci' ] ||
	fail 'device bound to the wrong driver'
grep -q 'vams-cq' /proc/interrupts || fail 'CQ MSI-X vector is absent'
grep -q 'vams-async' /proc/interrupts || fail 'async MSI-X vector is absent'
rmmod vams_pci || fail 'normal module unload failed'
[ ! -L "$vams_device/driver" ] || fail 'device remained bound after remove'

insmod /vams_pci.ko || fail 'second probe failed after full cleanup'
[ -L "$vams_device/driver" ] || fail 'second probe did not bind'
rmmod vams_pci || fail 'second remove failed'

finish 0
