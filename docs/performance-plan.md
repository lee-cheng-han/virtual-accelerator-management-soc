# Stress and performance qualification

VAMS performance evidence is produced entirely by its virtual platform. Host
timing is useful for regression detection and control-plane analysis, but is
not a prediction of physical silicon throughput. A correctness failure always
invalidates the timing results from that run.

## Automated qualification

`make stress-qualification` combines four reproducible checks:

- the independent SQ/CQ property model exercises wrap, backpressure, malformed
  doorbells, interrupt state, queue enable transitions, and reset;
- the bounded-DMA test verifies a 16 MiB copy and independent CRC plus
  cross-chunk fill/vector integrity, then reports host-clock throughput;
- the Zephyr boot test captures static SRAM use, every firmware-task stack
  high-water mark, fixed-pool and pipeline-queue occupancy, and the worst
  observed watchdog pet interval and margin;
- the stress runner completes one million commands, including all four payload
  opcodes, at queue occupancy 15/16, performs 1,000 deterministic queue resets,
  advances 24 hours of virtual time, and proves post-endurance liveness.

The stress runner checks every command ID, status, error, byte count, CRC where
applicable, and user cookie. It rejects missing, reordered, stale, duplicate,
or corrupt completions. Payload results are independently checked. Its JSON
reports are written to `build/reports/stress-qualification.json` and
`build/reports/firmware-resources.json`; they record counts, resets, wrap and
queue high-water, opcode mix, firmware resource margins, failures,
commands/s, and host-clock latency distributions (minimum, mean, standard
deviation, p50, p90, p99, p99.9, and maximum).

Use `make stress-smoke` for a short development run of 10,000 commands, 32
resets, 16 payload samples, and one virtual hour. Full-run parameters can be
overridden without modifying the test, for example:

```sh
make stress-qualification \
  VAMS_STRESS_COMMANDS=2000000 \
  VAMS_STRESS_RESETS=2000 \
  VAMS_STRESS_PAYLOAD_SAMPLES=512 \
  VAMS_STRESS_VIRTUAL_HOURS=72
```

## Clock domains and interpretation

NOP latency is measured per full queue batch from SQ-tail publication through
completion readback; it is not presented as single-command latency. Payload
latency includes explicit virtual-time engine advancement. Throughput and
latency use the host monotonic clock, while endurance uses QEMU virtual time.
Every report states this scope. No numerical pass threshold is inferred from a
developer workstation; a pinned CI runner must establish and version a
statistically controlled regression baseline.

Firmware resource lines are emitted after all service tasks have made progress.
Static SRAM is derived from the linked image bounds. Stack use comes from
Zephyr initialized-stack scanning. Pool and message-queue values are retained
high-water marks, and watchdog margin is the configured timeout minus the
largest observed successful pet interval. These measurements justify later
stack/pool tuning but do not replace overload and memory-pressure testing.

## Controlled benchmark extension

Release benchmarking adds warm-up and at least five measured repetitions for
64 B, 256 B, 1 KiB, 4 KiB, 64 KiB, 1 MiB, and 16 MiB payloads where legal. It
records the exact commit, QEMU/Zephyr/compiler/kernel versions, CPU model, host
load, VM topology, ring depth, opcode, timeout, interrupt/coalescing settings,
and random seed. CPU affinity and a fixed governor may be recommended but are
never changed silently by the scripts.

Future reports also cover firmware acceptance-to-publication latency, engine
utilization, scheduler delay, interrupt-to-thread latency, recovery duration,
DMA bytes, host/QEMU CPU time, polling wakeups, log drops, and telemetry
saturation. Raw artifacts and failures remain visible; averages cannot hide an
outlier or recovery error.
