# Command lifecycle

The lifecycle applies to one firmware command object. Ring slots have separate
producer/consumer ownership and must not be confused with command state.

```text
FREE --capture--> SUBMITTED --> VALIDATING --valid--> QUEUED --> RUNNING
                              | invalid                 |       |
                              v                         |       +--timeout--> ABORTING
                        COMPLETED_ERROR                 |       +--reset----> CANCELLED
                                                       v
                                                   COMPLETED
```

Terminal results wait in the completion service until CQ publication, then the
object returns to FREE. `COMPLETED_ERROR` includes validation and execution
failure; status/error fields distinguish them.

## Transition ownership and invariants

| From → to | Owner / trigger | Required action |
|---|---|---|
| FREE → SUBMITTED | Receiver after full descriptor DMA | Allocate object, capture generation/time/64 bytes, then advance SQ head. |
| SUBMITTED → VALIDATING | Receiver transfers message | Validator becomes sole object owner. |
| VALIDATING → COMPLETED_ERROR | Validator, first failed rule | Increment rejected; preserve captured ID/cookie; create INVALID result. No payload access. |
| VALIDATING → QUEUED | Validator | Reserve result capacity, increment accepted/high-water, compute absolute deadline. |
| QUEUED → RUNNING | Scheduler | Check deadline/generation, transfer object to DMA manager, program one engine slot. |
| QUEUED → ABORTING | Scheduler/recovery, deadline expired | No engine abort needed; create TIMED_OUT result through recovery. |
| RUNNING → COMPLETED | DMA manager, successful operation | Record bytes/result/latency and transfer result to completion service. |
| RUNNING → COMPLETED_ERROR | DMA/engine error | Stop transaction, record directional error; escalate if engine is not quiescent. |
| RUNNING → ABORTING | Recovery, absolute timeout | Block callbacks, request abort, wait bounded grace. |
| ABORTING → COMPLETED_ERROR | Recovery, abort acknowledged | Produce TIMED_OUT with zero bytes; destination unspecified. |
| any accepted → CANCELLED | Recovery, generation-changing reset | Invalidate callbacks and either publish RESET if CQ survives or notify host through reset boundary. |
| terminal → FREE | Completion service or reset reconciliation | Publish CQ entry before release, or let driver cancel old generation; zero object before pool return. |

No object may be visible in two task queues. Only a terminal-state result can be
published, exactly once. A callback carries the captured reset generation and
is discarded if it differs from current generation.

## Exceptional paths

**Validation failure.** SQ head already advanced because capture succeeded.
Firmware posts one INVALID completion. Later descriptors continue.

**SQ full.** The host cannot publish into the reserved empty slot. Driver
returns `-EAGAIN` or blocks; firmware state is unchanged. A malicious doorbell
that over-advances is rejected, sets queue error, and can trigger queue reset.

**CQ full.** Completion service retains bounded result objects, asserts stalled
status, and stops SQ receive at its watermark. It never overwrites CQ. A host
CQ doorbell resumes publication. Persistent stall is host malfunction, not a
firmware timeout for already completed work.

**Command timeout or engine hang.** Timeout includes QUEUED and RUNNING. Queued
work can time out without engine reset. Running work enters ABORTING; failed
abort escalates to engine reset. Later queued commands run only after engine
health is restored and their deadlines remain valid.

**DMA failure.** Payload DMA produces a directional FAILED completion when the
CQ path works. Descriptor-fetch failure has no trustworthy ID/cookie and causes
queue recovery. CQ-write DMA failure makes the queue unreliable and escalates
to queue/device recovery.

**Firmware restart.** SRAM command state is lost. Boot reports a new generation
and reset reason, initializes rings disabled, and does not replay descriptors
whose SQ head may have advanced. Driver cancels all old-generation requests and
reinitializes both rings.

**Host reset.** Driver blocks new work and masks interrupts. Hardware stops new
DMA, quiesces/cancels outstanding operations, increments generation, resets the
selected scope, and signals reset done. Driver never trusts pre-reset CQ tail.

**Lost completion interrupt.** CQ tail remains authoritative. Poll-on-wait and
periodic outstanding-work polling drain it and acknowledge the sticky source.

**Submitting process exit.** Driver marks the request orphaned but retains its
mapping until completion/reset. It drains and discards the completion. Exit does
not cancel a running device command merely by dropping user references.

