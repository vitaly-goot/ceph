Write-only EC journal experiment
===============================

Status
------

Initial implementation on ``wip/ec-journal-poc``, based on
``0abd6120a9d045623faa1a09f91b7374b8153f6b`` (``aka_version_20.2.4``).

Core committed as ``35d449d1881``; initial backend checkpoint ``a582e6e955d``.
The implementation connects it to the
optimized classic EC backend: admission, durable append, metadata commit,
timer/pressure drain, deferred data writes, trim, and PG-query counters.
It is disabled by default and restricted to an explicitly configured pool.

**Not deployment- or benchmark-validated.** Focused tests cover the journal
state machine and real EC read planner/stripe assembly. The backend translation
unit is compile-checked, but a fresh linked OSD build and disposable-cluster
test are still required. No measured durability or performance result exists.
Review regressions additionally cover real head-write attributes, distinct
size-option variants, submitted object-state snapshots, log admission barriers,
flush readiness and journal-to-production-assembler generations. Backend,
planner and OSD-shutdown translation units are checked with ``-Werror``.
These checks do not establish runtime correctness of the driver or shutdown.
Current focused result: **101 tests pass with GCC 13, ASan and UBSan**;
OSD-option translation, shell syntax and patch whitespace checks also pass.

Scope
-----

* Supported test profiles: 4+3, 8+3 and 12+3, with a verified 4096-byte stripe
  unit. Full data stripes are 16384, 32768 and 49152 bytes respectively.
* 1024- and 4096-byte random overwrites of precreated, prefilled objects only.
* No client-read overlay. Reads while data is journaled are unsupported in the
  eventual experiment. Verify data only after stopping writes and draining.
* Primary-local ObjectStore payload commit plus normal distributed metadata/log
  commit before ACK. This is not replicated durability of the client payload.
* No replay, failover, peering, recovery, scrub, backfill, snapshots, or format
  compatibility guarantees. Use disposable data and a fixed healthy cluster.

Implemented component
---------------------

``src/osd/ECWriteJournal.{h,cc}`` provides:

* Versioned records containing sequence, object, version, object size, offset and exact
  payload. No 4 KiB padding/preread for a 1 KiB append.
* Append-only ObjectStore writes to per-PG segment objects. The caller owns the
  collection, collision-free internal object prefix, submission and callbacks.
* Separate append and durable-commit states. Merely constructing/queueing a
  transaction does not make records eligible for flushing or client ACK.
* Bounded encoded bytes, record count, segment size and segment count. Capacity
  is released only after segment removal commits, not on client ACK or flush
  submission. A caller receiving ``-EAGAIN`` must seal/drain and apply
  backpressure; it must not bypass the journal with a newer write.
* Sparse 1 KiB-granular coalescing. Latest append sequence wins even if durable
  callbacks arrive in a different order. Repeated writes do not fill holes.
* Sealed flush generations. The oldest sealed, fully durable segment is the
  only one eligible to flush. Newer writes occupy different segments and cannot
  be erased by an earlier flush. Coalescing is within a segment, not across
  sealed generations.
* Complete-stripe assembly without base data. Incomplete stripes explicitly
  report missing logical ranges and require base data; holes are never silently
  zero-filled. Object tails request only bytes before EOF; encoding padding
  beyond the recorded EOF is zero. In particular, a 4 MiB RBD object does not
  end on a 12+3/4 KiB stripe boundary. The flusher must preserve logical size
  when writing the padded encoding.
* Segment removal prepared only on successful completion of all covering base
  EC writes. Flush/trim errors retain the journal; append errors stop admission.

All methods run under the owning PG lock. Buffers are shared and must remain
immutable. The caller must submit every successfully prepared transaction and
translate its result into the appropriate state transition. ``committed()``
and ``trimmed()`` belong in ``on_commit``, not ``on_applied``. Direct store
callbacks need ``Listener::bless_context()`` for PG locking/lifetime protection;
callbacks invoked by ``finish_rmw`` already hold the PG lock. No restart or
PG-reset lifecycle is implemented: a new instance must not reuse existing
segment names or claim their data was replayed.

The backend seals on timeout, pressure, rotation, explicit drain, or completion
of a dirty stripe in the open segment. A flush can expose several stripes;
removal waits for all of them. The initial driver submits one stripe at a time
per PG, in segment order. This deliberately limits concurrency for the POC.

Backend path
------------

``ECBackend::submit_transaction()`` recognizes single 1/4 KiB overwrites of
existing head objects with unchanged size and the normal ``OI_ATTR`` and
``SS_ATTR`` updates produced by ``finish_ctx``. Nonempty snapshot history is
not enrolled. Other transactions fall back to the original path, but only after
dirty journal data drains. Preparation/creation and unsupported compound
transactions should not be included in measured journal admission counts.

Projected OBC metadata is copied at submission, before another client can
project a newer size. Deferred planning uses that copy; the original size still
comes from the extent cache/attribute cache advanced by earlier admitted ops.
``call_write_ordered`` callbacks pass through the same admission queue before
being forwarded to the ordinary extent-cache ordering mechanism. This prevents
error log entries overtaking an older write waiting on journal pressure.

For eligible requests, it prepares the primary journal append and removes the
data buffer update **before** the ordinary EC planner runs. The normal
``ECClassicalOp`` writes OI and PG log metadata to all shards and appends the
journal transaction only on the primary. Its existing durable commit gather
then marks the journal record committed and invokes the original client
completion once. This retains remote metadata coordination; it does not claim
the latency of a purely local ACK.

``ECJournalFlushOp`` is a separate, zero-version, data-only pipeline operation.
``ECJournalFlush`` uses the existing conventional EC planner for missing/partial
pages, overlays journal bytes, supplies only known EOF zeros, and encodes all
shards. It does not resubmit foreground versions or change logical object size.
The normal extent cache and subwrite durable gather order physical writes.
Only after every stripe completes is the primary segment removed and its
capacity released by a blessed trim-commit callback.

Callbacks inside ``finish_rmw`` already run under the PG lock; directly queued
ObjectStore trim callbacks use ``bless_context``. The PG intrusive timer owns
PG references and schedules work without recursive completion callbacks.
Append commits wake the driver only when the oldest sealed segment becomes
fully durable. Flush/trim commits also wake progress. Only an actually open
segment has a deadline timer; sealed/flushing segments do not re-arm it.

**Ordering limitation:** background flush operations share the normal ordered
RMW commit queue. A slow flush can hold up ACKs for later metadata-only writes.
Thus this first driver tests coalescing and aggregate read savings, not fully
independent foreground/background latency. Timer dispatch and one-stripe-at-a-
time flushing also impose overhead which must be reported, not hidden.
The admission FIFO is retained for bounded journal pressure and drain-before-
unsupported-operation barriers. This version fixes its ordering contracts;
it does not remove cross-object head-of-line blocking or introduce independent
foreground/background pipelines.

Experimental controls
---------------------

Startup settings (all relevant OSDs must run the modified binary):

* ``osd_ec_journal_poc_enable``: false by default.
* ``osd_ec_journal_poc_pool``: numeric test-pool ID; -1 disables enrollment.
* ``osd_ec_journal_poc_segment_bytes``: 4 MiB per-PG open-segment limit.
* ``osd_ec_journal_poc_max_bytes``: 16 MiB per-PG total encoded journal budget.
* ``osd_ec_journal_poc_max_records``: 8192 records per PG.
* ``osd_ec_journal_poc_flush_ms``: 100 ms coalescing window.

The pool must use ``allow_ec_overwrites`` and ``allow_ec_optimizations`` and a
4+3, 8+3 or 12+3 profile with 4096-byte chunks. Prefill the test data with journaling
off or draining, then stop clients before starting the measured phase.
Do not enable on any existing production pool. Budgets are per PG, not per OSD.
Startup enrollment settings are read once per backend. Unsupported geometry
or invalid capacity limits log an error and refuse enrollment rather than
throwing/asserting. Size options are read as ``Option::size_t``. A record too
large for a configured segment falls back after draining older records.

``osd_ec_journal_poc_drain`` is a runtime boolean: stop writers and set it on
all participating OSDs. It prevents further enrollment and drains pending
segments. Wait for PG-query ``ec_write_journal_poc`` counters ``bytes``,
``records``, ``pending_admissions`` and ``flush_in_progress`` to reach zero
before reading data or ending the experiment. Reads while dirty are neither
overlaid nor rejected: running them would return stale data.

The query also reports ``admitted``, ``acked``, ``full_stripes``,
``partial_stripes``, ``planned_flush_read_bytes``, ``flushed_logical_bytes``
and ``pressure_events``. Planned bytes precede cache lookup and are not physical
device reads. Measure device reads separately. Journal-full requests wait in
the admission queue without ACK, under the OSD's existing client throttles.
``pressure_events`` counts requests that first encounter journal capacity
pressure, not repeated retries or timer wakeups for the same request.

Graceful OSD shutdown (including the fast-shutdown path) first stops local
enrollment and requests drains while workers and cluster messaging are live,
before notifying the monitor or entering ``STATE_STOPPING``. The shutdown
thread waits outside PG/OSD locks, at most 30 seconds; timeout/cancellation
refuses shutdown with ``-EBUSY`` and keeps drain enabled. Retrying is possible
after the backlog clears. This has compile coverage but still needs a real
signal-driven OSD test. An external supervisor's forced kill is not protected.

**Drain all participating OSDs before stopping peers or the entire cluster.**
Local shutdown draining does not coordinate other primaries' journals: taking
away their shards can cause an unsupported dirty peering reset. Dirty PG reset
still deliberately aborts: no replay or failover is implemented.
Live segment objects are registered in ``temp_contents`` and unregistered
after trim commit. On restart ``OSD::clear_temp_objects`` also removes temp-pool
objects regardless of that in-memory registration, so they are not permanent
orphans. This is discard, **not** recovery of old journal records.
After any crash/reset discard the test data; do not treat a restart as a valid
durability test. No deployments or changes to the historical fleet are made by
this patch.

Validation
----------

Normal core build target: ``unittest_ec_write_journal``. It links the small journal
component rather than the entire OSD backend. The tests cover all three geometries,
both write sizes, exact ObjectStore record encoding, commit-before-flush,
overlaps, duplicates, holes, independent objects/stripes, capacity, failure
handling, and writes arriving while an older generation drains.

``unittest_ec_journal_flush`` additionally exercises the actual EC planner and
shard extent mapping for all three geometries: full/partial pages, complete stripes,
nonzero offsets, unaligned 4 KiB writes, sparse fragments, EOF padding,
metadata-only foreground plans, and missing-base assertions.
Its admission tests use the same option extraction, eligibility, FIFO barrier
and OBC snapshot helpers as the backend. Generation tests fold real journal
records and run ``plan_flush``/``assemble_flush`` against nonzero base data.
``Stripe::holes`` and ``Stripe::assemble`` remain explicit reference-model
helpers, not a claim that the OSD uses that implementation.

``src/script/test-ec-write-journal-poc.sh`` offers a focused build with an
existing compatible Ceph build. Set ``CEPH_DEPS_SOURCE``, ``CEPH_DEPS_BUILD``,
``CXX`` and, if necessary, ``BOOST_ROOT``. Run the script with bash; pass gtest
arguments as script arguments. ``EC_JOURNAL_SANITIZE=1`` enables ASan/UBSan for
the new code and tests (the reused Ceph shared library is not instrumented).
The dependency checkout is read-only; artifacts go under ``build-journal-poc``
or ``EC_JOURNAL_TEST_BUILD``. This is not a replacement for a fresh full build.
``EC_JOURNAL_FLUSH_TESTS=1`` includes the EC planner/assembly suite;
``EC_JOURNAL_BACKEND_CHECK=1`` also compile-checks the actual backend, common
planner and OSD shutdown unit with warnings treated as errors. It requires
``lttng-gen-tp`` and generates missing trace headers only in the test output
directory. The focused flush build discards unrelated EC
transaction sections to avoid linking the entire OSD. It does not test the
complete OSD or replace its CMake build.

Commit notifications are simulated, **not a BlueStore durability test or
running-cluster benchmark**. The current flush tests assemble data shards but
do not validate plugin parity generation or the distributed commit path.

Remaining integration
---------------------

1. Fresh linked ``ceph-osd`` build and normal CMake test targets.
2. Disposable cluster test of foreground journal commit/ACK and deferred EC
  writes, including repeated overwrites, timer/pressure drain, a write arriving
  during a flush, and verification after complete drain. Confirm PG log and
  cache behavior under real subwrite callbacks; compile checks cannot prove it.
    Include pressure followed by a truncate and write-error log entry, plus
    graceful-stop drain and timeout/refusal with a delayed shard. A successful
    stop must follow full data commit and journal removal; a timeout must not
    proceed to OSD teardown.
3. Validate real plugin parity against the ordinary write path, and inspect
   physical reads plus the OSD request-size histogram.
4. Measure the matrix below, including head-of-line blocking and serialized
   flusher overhead. Increase flusher concurrency only after correctness of the
   healthy write/drain path is demonstrated. No client-read overlay.

Benchmark matrix and accounting
-------------------------------

Run baseline and journal versions for each of:

=========== ========== =================
Profile     Write size Full data stripe
=========== ========== =================
4+3         1 KiB      16 KiB
4+3         4 KiB      16 KiB
8+3         1 KiB      32 KiB
8+3         4 KiB      32 KiB
12+3        1 KiB      48 KiB
12+3        4 KiB      48 KiB
=========== ========== =================

Keep the initialized dataset, working set, random stream, queue depth, plugin,
adaptive PDW settings, PG count and placement identical per comparison. Verify
actual OSD write sizes: client layers may merge or reject 1 KiB requests.
Use a broad uniform-random workload as the main test. A separate, explicitly
labelled stripe-completing workload can validate the no-read mechanism.

Measure foreground ACK latency separately from drain work: IOPS, p50/p99,
foreground/flush logical EC reads, physical device reads/writes, serialized
journal bytes, complete/incomplete flush counts, coalescing ratio, backlog/age,
CPU, device queue depth, and final drain duration. Run beyond journal capacity
repeatedly and require a stable bounded backlog. Include journal writes and
final draining in end-to-end amplification/throughput.

Uniform random writes may not complete stripes within a bounded window. Then
the journal defers/coalesces RMW rather than eliminating it and can increase
write amplification. Four 1 KiB writes filling a page do not fill a 16/32/48 KiB
stripe. Do not claim success solely from faster ACKs and a growing backlog.