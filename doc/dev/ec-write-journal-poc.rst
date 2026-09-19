Write-only EC journal experiment
===============================

Status
------

Initial implementation on ``wip/ec-journal-poc``, based on
``0abd6120a9d045623faa1a09f91b7374b8153f6b`` (``aka_version_20.2.4``).

**The journal component and unit tests exist; the EC backend does not yet use
them. There is no enable flag, changed client ACK path, or performance result.**
This is intentionally not an incomplete early-ACK hook into the running OSD.
The PG log/version/completion integration must be implemented together before
an OSD may acknowledge writes from this journal.

Scope
-----

* Primary target: 8+3, also test 12+3, with a verified 4096-byte stripe unit.
  Full data stripes are 32768 and 49152 bytes respectively.
* 1024- and 4096-byte random overwrites of precreated, prefilled objects only.
* No client-read overlay. Reads while data is journaled are unsupported in the
  eventual experiment. Verify data only after stopping writes and draining.
* Primary-local ObjectStore commit before ACK, not replicated durability.
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
and ``trimmed()`` belong in ``on_commit``, not ``on_applied``. Wrap callbacks in
``Listener::bless_context()`` for PG locking/lifetime protection. No restart or
PG-reset lifecycle is implemented: a new instance must not reuse existing
segment names or claim their data was replayed.

The current policy seals on explicit request or segment rotation. A backend
driver still needs timeout, pressure and full-stripe flush scheduling. A flush
can expose several stripes from a segment; removal must await every one.

Validation
----------

Normal build target: ``unittest_ec_write_journal``. It links the small journal
component rather than the entire OSD backend. The tests cover both geometries,
both write sizes, exact ObjectStore record encoding, commit-before-flush,
overlaps, duplicates, holes, independent objects/stripes, capacity, failure
handling, and writes arriving while an older generation drains.

``src/script/test-ec-write-journal-poc.sh`` offers a focused build with an
existing compatible Ceph build. Set ``CEPH_DEPS_SOURCE``, ``CEPH_DEPS_BUILD``,
``CXX`` and, if necessary, ``BOOST_ROOT``. Run the script with bash; pass gtest
arguments as script arguments. ``EC_JOURNAL_SANITIZE=1`` enables ASan/UBSan for
the new code and tests (the reused Ceph shared library is not instrumented).
The dependency checkout is read-only; artifacts go under ``build-journal-poc``
or ``EC_JOURNAL_TEST_BUILD``. This is not a replacement for a fresh full build.

These are state-machine tests with simulated commit notifications, **not a
BlueStore durability test, EC encode test, or running-cluster benchmark**.

Remaining integration
---------------------

1. Restrict enrollment to eligible writes in a designated experimental pool,
   before ``ECBackend::submit_transaction()`` starts prerequisite reads.
2. Keep PG log, projected/durable versions, object contexts, stats and ordered
   completion coherent with local journal commit and deferred data writes.
   Do not invoke the normal all-shards-committed callback twice or advance
   base-data commit state merely because the primary journal committed.
3. Implement a driver for append submission and blessed commit callbacks,
   bounded admission, timeout/full-stripe/pressure triggers and explicit drain.
4. Submit coalesced stripes through existing EC encoding/subwrites, preserving
   cache coherence. Partial coverage must retain internal RMW/base reads;
   handle EOF without changing logical object sizes.
5. Wire data-commit and trim-commit callbacks, instrumentation, and controlled
   unsupported-operation/PG-reset handling. No client-read overlay.
6. Build the OSD, run a disposable-cluster smoke test, then the matrix below.

Benchmark matrix and accounting
-------------------------------

Run baseline and journal versions for each of:

=========== ========== =================
Profile     Write size Full data stripe
=========== ========== =================
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
write amplification. Four 1 KiB writes filling a page do not fill a 32/48 KiB
stripe. Do not claim success solely from faster ACKs and a growing backlog.