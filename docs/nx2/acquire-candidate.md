# EGLA-42 acquire candidate

This branch is an experiment, not a production merge candidate. It is based on
`origin/nx2` at `412c753b3a37a6b1d1459da3052dcff96d807af1` and does not merge upstream.

## Runtime contract

`VideoCore::Nx2Debug::g_sem_fast` is an `atomic<int>` initialized to `-1`.
`SwitchValue(g_sem_fast, "EDEN_NX2_SEM_FAST", 0)` reads its startup default lazily.
The parent package supplies debug-server dispatch.

| Value | Pending-acquire behavior |
|---|---|
| 0 | Drain and read coherently on every poll. |
| 1 | Resume without draining only when the existing waiter registry has latched a completed release. Otherwise use mode 0. |
| 2 | As mode 1, but defer an unlatched poll at most 15 times and only while less than 1 ms has elapsed since the last drain. Then drain and read coherently. |

The setting is read once at the start of each pending-acquire poll on the GPU
front thread. A live change takes effect at the next poll; an in-flight poll
finishes under its previous value. Disabling it needs no cache invalidation or
reconstruction of writer history. The next unlatched poll drains normally.
Initial acquires, registration, equality/GEQUAL/mask predicates, releases and
buffer-cache flushes retain their existing paths. Deferrals publish pending
work and request the back's idle hook. The 2-second warning never disables the
poll/time bound.

Mode 2 can delay observing a CPU release until the next bounded coherent poll.
CPU writes are not latched by this registry, so reducing their sampling rate can
also miss a transient equality/mask match. Mode 1 does not defer these reads.
The 1 ms threshold is checked when a poll runs, not a real-time scheduling
guarantee. Neither mode predicts a release from queued work or reads a raw guest
word before draining. Neither mode is claimed faster without gameplay data.

With `EDEN_NX2_GPU_PROFILE=1`, the existing per-thread report adds these rates:
`AcquirePolls`, `AcquirePollsLatched`, `AcquirePollsDeferred`,
`AcquirePollsDrained`, and `AcquireFenceRetries`. The log calls them `polls`,
`latched fast`, `acquire polls deferred`, `drained`, and `fence retries`.
Existing acquire/fence-release drain counts and wait times remain available.

## Source audit

The full history was fetched before merge-base comparisons. PR #179's closing
note says its switches cannot merge to nx2; it is not a correctness approval.
Its Linux verification comment found fast/drained acquire disagreements with
both overlay and no-overlay variants. PR #231 later corrected its original FPS
claim to about 1.5 FPS in a controlled comparison. Its Mario Kart World report
identified a permanent no-drain path after `reported` became true.

| Source | Disposition |
|---|---|
| upstream `90ff431884ddb875882ed084f4e76538bfb2df68`, kraibse | Cherry-picked with `-x`. Retry delayed fence releases at most once per millisecond while suspended. This applies in all modes, so mode 0 is nx2 plus this dependency. Current nx2's fence manager already submits deferred work before owner-thread waits. |
| #231 `475c498c1a24fcfb785cfcc79da1703237c2d5fa`, PantACRO4life; fork alias `547ce11b4f` | Cherry-picked with `-x`, retaining only the completed-latch shortcut and relevant counters. Removed the speculative-word and initial-acquire drain-elision paths. |
| #231 `b837e742c46b0ab8cb3ffad4db2dc6791cb59291`, PantACRO4life; fork alias `f362e5a5fd` | Cherry-picked with `-x`, adapting the 16-poll/1-ms bound to defer only negative pending decisions, without speculative memory reads. |
| #179 `a281376cc8` through `166c21d96c`, thibaut | Audited, not required by the narrowed candidate. No queued-payload overlay, async slot map, run-cache replacement, DMA optimization or unrelated debug work is carried. |
| `22aad455dc`, `1c9a048e9d`, Frizti | Not carried. GPU-virtual ranged waits/forwarding cannot establish physical-alias safety; same-address-space matching does not rule out another virtual alias. |
| `b65e2bcc87`, `abbe465fe0`, `9932d05c4a`, Frizti | Not carried. A queued release is not proof that its fence-carried write has completed. The SEM_EXECUTE change also removes the existing synchronous wait without establishing an equivalent ordering contract. |
| `99b3b49595`, Frizti | Already represented by current nx2's `DmaPusher::IsMemoryWriter` from #194. Preserved. |

The concrete #179 hazard is `NoteSemaphoreWrite`: a short fence release is
recorded with its known payload, so `OverlayQueuedWrites` can satisfy an acquire
before `QueryCacheBase::CounterReport` executes its fence callback. Current
Balanced/Accurate fence modes defer that callback. A ring-consumed position is
not a completion fence. The candidate instead uses `Nx2SemaphoreWaiters::OnWrite`,
which the current query and native compute completion paths call after writing
the release value.

Upstream's scheduler-owner changes were inspected and not imported over nx2's
newer owner-thread `Run` calls. Unmerged upstream PR #712 was rejected: its
equality-as-GEQUAL and synthetic ring-rescue writes alter guest semaphore
semantics rather than proving completion.

## DMA snapshot extension withheld

`8ac3666f7b`, `526b9f240a`, and `28b262b59d` were audited separately. The snapshot
hit predicate can ignore a graphics write and a later host-sync mark:

1. `Commit(Effect::Gpu)` updates the fork's `SplitWriteTracker::gpu` position.
2. `NoteSync` updates `last_sync_position`.
3. Snapshot `MayBeWrittenSince` checks neither field. It consults different,
   older compute/copy and memory-writer notes. The older 3D memory-write mask
   excludes draw methods.
4. A previously flushed page with no cached report can therefore hit after a
   draw writes its storage buffer. Mode 9 returns guest bytes without checking
   the buffer cache or draining; mode 10's later coherent read can differ.

Compiling those source functions with the source `SplitWriteTracker` reproduces
the false-clean answer. The source's separate alias-aware tracker is not used by
the snapshot predicate and is absent from current nx2. A useful safe port needs
complete draw/deferred-draw invalidation, physical-range alias handling and
pending-completion coverage. Invalidating on every queued entry would be safe
but would not preserve the claimed per-object drain reduction. No DMA snapshot
switch is exposed by this branch. The parent owns the independent fence-flush
timing experiment.

## Checks

`src/tests/video_core/semaphore_acquire.cpp` exercises the poll/time bounds,
including already-reported acquires, live disable, delayed-fence retry, completed latches that were
later overwritten, registration cleanup, and real split queue drain behavior.
Run it with `build/bin/tests '[semaphore_acquire]'`, then run the full test
binary and CTest suite. Cross-check both `puller.cpp` and the new test unit with
clang's `--target=aarch64-linux-gnu` using the ARM sysroot and target architecture
define, without x86-only compiler flags. These checks do not replace the parent's
gameplay measurements or establish visual correctness in the affected scenes.
