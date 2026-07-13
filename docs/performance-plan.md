# Performance measurement plan

Phase 0 contains no performance results. QEMU timing is useful for regressions
and control-plane analysis, not a prediction of physical silicon throughput.

## Metrics

- Command latency from host tail publication to CQ observation, reported as
  count, min, median, p90, p99, p99.9, max, mean, and standard deviation.
- Firmware latency from descriptor acceptance to CQ publication, correlated by
  command ID and device timestamp.
- Steady-state payload throughput and commands/s for each implemented opcode.
- Host CPU time, QEMU process CPU time, completion interrupts/command, polling
  wakeups, queue occupancy/high-water, timeout/error rate, and DMA bytes.
- Firmware task stack high-water, scheduler queue delay, engine busy fraction,
  and recovery duration by scope.

## Method

`vamsbench` will use monotonic raw host time, pre-fault mapped/pinned buffers, a
warm-up interval, then at least five measured runs. It records exact git commit,
QEMU/Zephyr/compiler/kernel versions, host CPU/model, governor, VM vCPU count,
ring depth, payload size, opcode, timeout, interrupt/coalescing/poll settings,
and random seed. Raw JSON/CSV and a human summary are retained.

Test sizes are 64 B, 256 B, 1 KiB, 4 KiB, 64 KiB, 1 MiB, and 16 MiB where legal.
Queue depths are 16, 64, 256, and 1024. Runs cover synchronous queue depth 1,
steady-state maximum safe occupancy, interrupt mode, lost-interrupt polling,
and recovery. CRC/vector tests initialize buffers outside the measured region;
every run samples or fully verifies output as appropriate.

Host load and unrelated processes are recorded. CPU affinity and fixed governor
are recommendations, not silently applied by scripts. QEMU virtual time must not
be compared directly with host wall time; each metric names its clock domain.
At least one baseline NOP run isolates control-plane cost.

## Acceptance and reporting

Phase 9 defines numerical regression thresholds from a checked-in baseline on a
named CI runner; Phase 0 invents none. A candidate regression is compared with
confidence intervals or repeated medians and is never hidden by averaging
failures. Correctness failures, timeouts, resets, or lost samples invalidate the
performance run and remain visible.

The final report separates measured QEMU behavior from design targets, includes
raw artifacts and commands, and explains bottlenecks. README numbers are added
only after the automated benchmark reproduces them.

