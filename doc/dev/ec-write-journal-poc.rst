Write-only EC journal experiment
===============================

Status
------

Initial implementation on ``wip/ec-journal-poc``, based on
``0abd6120a9d045623faa1a09f91b7374b8153f6b`` (``aka_version_20.2.4``).

Core committed as ``35d449d1881``; initial backend checkpoint ``a582e6e955d``.
The implementation connects it to the
optimized classic EC backend: admission,
durable append, metadata commit, timer/pressure drain, deferred data writes,
trim, and PG-query counters.
It is disabled by default and restricted to an explicitly configured pool.

**Not deployment- or benchmark-validated.** Focused tests cover the journal
state machine and real EC read planner/stripe assembly. The backend translation
unit is compile-checked, but a fresh linked OSD build and disposable-cluster
test are still required. Durability is not validated; the performance figures
quoted below are test-cluster measurements that guided the design.
Review regressions additionally cover real head-write attributes, distinct
size-option variants, submitted object-state snapshots, log admission barriers,
flush readiness and journal-to-production-assembler generations. Backend,
planner and OSD-shutdown translation units are checked with ``-Werror``.
These checks do not establish runtime correctness of the driver or shutdown.
Current focused result: **211 tests pass with GCC 13, ASan and UBSan**,
including detached record headers. The production
backend, planner, generator, journal core, PrimaryLogPG and OSD translation
units pass the focused warning-as-error compile check. These are not
measurements of BlueStore write amplification or end-to-end throughput.

**The record format (magic "ECJ6": the header carries the client mtime, and records are no longer PG-logged) is incompatible with previous builds.
Drain the journal (``osd_ec_journal_poc_drain``) until every PG has removed
its slot objects, or delete and recreate old experimental pools, before
running this build.** There is no legacy decoder or rolling-upgrade support.

Scope
-----

* Supported test profiles: 4+3, 8+3 and 12+3, with a verified 4096-byte stripe
  unit. Full data stripes are 16384, 32768 and 49152 bytes respectively.
* 1024- and 4096-byte random overwrites of precreated, prefilled objects only.
* By default only eligible writes likely to share a flush are journaled:
  those of stripes the journal already holds or that were written again
  recently. The rest take the ordinary EC write path (see "Admission
  control").
* No client-read overlay. A client read that overlaps journaled stripes waits
  while those stripes flush, then reads the shards (a read barrier). It is
  correct but slower than serving the bytes from the journal would be.
* Before ACK, the record is committed on shard 0 and every parity shard (m + 1
  copies by default) as an unlogged record: the write takes no version and
  no PG log entry, and the object's own state (version, mtime, data) is
  unchanged until the flush writes the data.
* Replay after a failover rebuilds the journal from the new primary's own
  slots and the other holders' slot listings, and fetches any record its own
  slots lack from a holder that has it. No snapshot support or format
  compatibility guarantees. Use disposable data.

Implemented component
---------------------

``src/osd/ECWriteJournal.{h,cc}`` provides:

* Versioned headers containing sequence, object, version, object size, offset
  and exact payload length. Payloads are padded to 4 KiB on disk without a
  preread; the header lives separately in the slot's OMAP.
* One stripe flush is an ordinary EC write, and several are kept outstanding
  per PG (``osd_ec_journal_poc_max_flushes``, default 8). Reclaiming a sealed
  segment means materializing every live stripe it still holds, and admission
  stops while the log is full, so flushing one stripe at a time bounded a PG's
  entire write rate at one round trip per stripe. On an 11 client, 55 OSD run
  that capped the cluster at 45k IOPS with a 838 ms p99 while only 54 of 1056
  cores were busy; the OSD queues were empty and hundreds of client writes sat
  waiting for room. Stripes are independent, so the flusher pipelines
  them and the journal keeps its own ordering guarantees per stripe. Raising
  the limit past the default bought little on that run (8 -> 32 gave +7% at
  QD128 and nothing at QD32): what remained at high queue depth was
  coalescing, not concurrency - see reclaim below.
* Append-only ObjectStore writes to per-PG segment objects. A segment occupies
  one of ``max_segments`` fixed slot objects, and ``append`` only *places* a
  record (slot, offset, encoded bytes); the caller emits the same bytes into
  the transaction of every shard that holds a copy (see "Replicated records"
  below), submits them and owns the callbacks. Every payload starts at a 4 KiB
  multiple and is zero padded to one. Both a 1 KiB and a 4 KiB payload occupy
  one data block; an 8 KiB payload occupies two. Headers are immutable OMAP
  entries committed in the same transaction, so each data append is a whole-block
  write into never-written space. Without that, BlueStore reads the device
  block the previous record ended in and rewrites the merged blocks through
  its deferred-write WAL: one device read and a doubled write per client
  write, and a 4000-extent onode per 16 MiB segment. Budgets count the padded
  payload plus header values and OMAP keys. Header and OMAP-update encoding
  is shared across all holders, with no per-record slot xattr update.
* Separate append and durable-commit states. Merely constructing/queueing a
  transaction does not make records eligible for flushing or client ACK.
* Bounded encoded bytes, record count, segment size and segment count. Capacity
  is released when a closed segment holds no live record and all its appends
  are durable, not on client ACK or flush submission. A caller receiving ``-EAGAIN`` must flush and apply backpressure;
  it must not bypass the journal with a newer write of a stripe the journal
  holds.
* An in-memory index of live 1 KiB blocks folded per stripe, one version per
  block. Latest append sequence wins even if durable callbacks arrive in a
  different order; a superseded block is dead immediately and its memory is
  released. Repeated writes do not fill holes.
* Stripe-granular flushing. A stripe is chosen, snapshotted and marked
  flushing; a record appended to it meanwhile has a newer sequence and stays
  journaled. Coalescing spans segments: a stripe's live blocks may come from
  several segments and one flush materializes them all.
* Complete-stripe assembly without base data. Incomplete stripes explicitly
  report missing logical ranges and require base data; holes are never silently
  zero-filled. Object tails request only bytes before EOF; encoding padding
  beyond the recorded EOF is zero. In particular, a 4 MiB RBD object does not
  end on a 12+3/4 KiB stripe boundary. The flusher must preserve logical size
  when writing the padded encoding.
* Segment release independent of flushing: a closed segment none of whose
  records is live (every one flushed or superseded) and whose appends are all
  durable is released at once, budget and slot. There is no removal
  transaction: the next segment placed in that slot removes the old contents
  in the same transaction as its first record, on every holder. Flush errors
  retain the journal; append errors stop admission.

All methods run under the owning PG lock. Buffers are shared and must remain
immutable. The caller must submit every successfully prepared transaction and
translate its result into the appropriate state transition. ``committed()``
belongs in ``on_commit``, not ``on_applied``. Direct store
callbacks need ``Listener::bless_context()`` for PG locking/lifetime protection;
callbacks invoked by ``finish_rmw`` already hold the PG lock. After a PG
reset the index starts empty; replay (below) adopts records read back from
the slots.

Flush policy
~~~~~~~~~~~~

The unit of flush is a stripe, not a segment. ``begin_flush()`` picks:

1. any complete, durable stripe: it needs no reads, so it goes out as soon as
   the flusher is idle, whatever the age policy;
2. otherwise, and only when the driver asks for incomplete stripes, the
   durable incomplete stripe with the oldest live record, strictly oldest
   first. That is also the order that frees the oldest segment.

The driver asks for incomplete stripes when the log has to reclaim its
oldest segment, when an unsupported mutation waits for the objects it touches
(then all of their stripes, ahead of anything else) or a drain for a clean
journal (then all of them), and when a stripe is older than
``osd_ec_journal_poc_flush_ms`` (0 disables that age limit). Candidates
blocked by a scrub range or a degraded object are passed over.

Reclaim is bounded by sequence, not just triggered by ``pressure()``.
``Journal::reclaim_bound()`` is the newest sequence of the oldest segment that
still holds live records, and is set only while the log would still be short
of room after every already-dead segment is removed. Since nothing older is
live, the stripes pinning that segment are exactly those whose oldest live
record is at or below the bound; ``begin_flush`` takes those and nothing
younger. ``pressure()`` itself goes true when the last segment opens and
stays true until a segment is *released*, so flushing by it alone keeps
taking stripes oldest first after the oldest segment's stripes are all in
flight, while that segment's last flushes complete. Each of
those extra stripes is flushed incomplete for no capacity gain, and with
several flushes in flight the flusher can reach several segments deep.

That over-reach was not what held coalescing down on the test cluster,
though. On the 11 client, 55 OSD cluster at QD128 the logs averaged 9-11 of
16 segments and only 56% of flushed stripes were complete (5.9 client writes
per flushed stripe), against the roughly 94% that a 15-segment window gives
uniform 4 KiB writes, and bounding reclaim left both numbers unchanged
(55%). Per PG the picture was bimodal: a third of the PGs held 15 segments
and flushed 99.8% of their stripes complete, while busier ones repeatedly
built up to about 7 segments and then fell to 1 within seconds, flushing
every stripe. Those collapses were PG-wide unsupported-operation barriers.
fio issued only 4 KiB writes, but librbd's default I/O scheduler
(``rbd_io_scheduler=simple``) merges adjacent writes to one object while an
earlier write to it is in flight, and the resulting 8 KiB requests were not
enrolled. The more writes are in flight per object, the more merges, which is
why coalescing fell with queue depth. Whole-block writes within one stripe
are now enrolled, and an unsupported mutation flushes only the objects it
touches (see the backend path below).

The first version sealed a whole segment on a timer and flushed everything in
it, complete and incomplete alike, and kept every record (including
superseded ones) in memory until the segment was trimmed. With uniform 4 KiB
random writes the fraction of complete stripes is then a function of the
window only: 42% at a 300 s window on the test cluster, with the whole PG
flushing at once when the window expired. Under the stripe policy the
retention of an incomplete stripe is bounded by the log size rather than by
wall-clock time: a record is reclaimed only when its segment is the oldest of
a full log, by which time most of the stripes it belongs to have completed or
been overwritten. Smaller segments in the same budget keep incomplete stripes
longer (the oldest of *n* segments has aged *n-1* segment fills) and turn the
reclaim into a trickle instead of a storm. Memory holds one version per live
block, so it is bounded by the working set rather than by the record rate.

The driver keeps up to ``osd_ec_journal_poc_max_flushes`` stripe flushes in
flight per PG, each its own EC write.

Backend path
------------

Admission is decided in ``PrimaryLogPG::do_op``, before an op is given a
version (``PGBackend::ec_journal_admit``). A write is eligible for the
journal when it is a single plain ``WRITE`` of whole 1 KiB blocks within one
stripe (1 KiB and 4 KiB client writes, and the 8 KiB or larger requests
librbd makes by merging adjacent ones) of an existing head object without
snapshot state, leaving its size unchanged, while the acting set holds every
copy and no drain is requested (``ECWriteJournal::candidate_write``, which
predicts what ``eligible_overwrite`` accepts at submission). Admission
(below) decides which eligible writes the journal takes; one it sends to the
ordinary EC path writes a stripe that holds no record and runs at once,
beside the object's journaled stripes. Any other write of an object that
still has journaled stripes waits in do_op until they have flushed, complete
or not; a write the journal has to take (its stripe holds records) that
finds the log full waits there too, until a segment is released. Later ops of a waiting object queue behind it (reads
included), so an object's ops keep their order. Waiting ops are requeued by
``ec_journal_kick`` once their object is clean or room has come back, and on
an interval change.

An admitted write never reaches ``execute_ctx``: do_op hands it to the
backend (``PGBackend::ec_journal_write``) in the same PG-lock critical
section, with no ``OpContext``, no version, no log entry and no object lock.
``ECBackend::journal_write`` places the record, gives it a *journal
version* (the interval's start epoch and the sequence the append takes, in
an ``eversion_t`` of its own version space, so that versions and sequences
order alike), and sends one unlogged ``ECSubWrite`` per holder carrying
that holder's copy of the record: the slot write, the OMAP header and, on a
segment rotation, the slot reset. The holder (``handle_sub_write`` with
``unlogged``) queues that transaction and replies on commit; it logs
nothing, updates no stats and applies no object state. The primary keeps
the write in a per-tid table until every holder has replied, then marks the
record durable and acknowledges the client in order per object
(``Listener::ec_journal_acked``): the reply carries the object's current
versions, which the write did not move. Writes to different objects
complete independently of each other and of the RMW pipeline, so a
journaled write never queues behind a flush's reads or its eleven-shard
commit. Whole-object writes that record a data digest are not enrolled,
because a scrub between ACK and flush would compare that digest with
unflushed shards.

Not being PG-logged changes three things. A client's retry after a failover
is executed again instead of being answered from the log's dups, which is
idempotent for the whole-block overwrites of an RBD image. The object's
mtime and user_version move at the flush; the record carries the client's
mtime and the flush applies the newest one of its stripe. And peering never
rolls a record back: a record of a write that was never acknowledged is
applied at replay like any other unless a flush or a later direct write
covered it, which is what the client's resend writes again anyway.

Admission
~~~~~~~~~

``ECBackend::journal_admit`` decides, for each eligible write:

1. A write of a stripe that is resident or flushing in the journal
   (``Journal::dirty(object, stripe, width)``) is journaled (``admit_live``):
   nothing may overtake journaled data. If the log is full it waits for room.
2. Otherwise a whole aligned stripe goes direct (``bypass_full``): it needs
   no read, so the journal saves nothing.
3. Otherwise it is journaled (``admit_new``), unless the log is full: then it
   goes direct instead of waiting for room (``bypass_room``).

No write of an object whose records replay has yet to fetch or judge goes
direct.

A direct eligible write needs only its own stripe to be clean, which rule 1
guarantees, so it does not wait for the object's other journaled stripes;
they keep their records and their flushes. Other writes keep the object
barrier described above.

Replay needs a narrower marker for such a write. A record of stripe ``s``
with version V is materialized when ``V <= max(base, stripes[s])`` (see
"Replay"), and a direct write of an object without live records sets
``base`` to its own version. Beside live records, that ``base`` would cover
the other stripes' unflushed records, and a replaying primary would drop
them. Such a write (``PrimaryLogPG::finish_ctx``, when
``ec_journal_object_dirty``) therefore sets only ``stripes[s]`` to its
version for each stripe it writes, raises ``base`` no further than just
below the object's oldest live record (``Journal::oldest_version``; no
record of the object below that is live) and marks the transaction
``ec_journal_marker_delta``. The EC transaction generator merges that delta
into the cached attribute in write order, as it merges a flush's (``base``
and each stripe take the maximum, then entries at or below ``base`` are
dropped), so earlier markers stay, including those of flushes queued with
the write. A record journaled on the written stripe later has a newer version
and stays uncovered.

The PG query adds ``admit_live``, ``admit_new``, ``bypass_full`` and
``bypass_room``. They count admission attempts, not necessarily submitted
writes: ``journal_admit`` runs before the per-object FIFO and object-lock
checks, so an operation retried after either wait is counted again.

Up to build -57 a recency table (``HotStripes``, tuned by
``osd_ec_journal_poc_admit_wpf`` = N) also sent cold stripes direct: a
stripe without records was journaled only if it had been written within the
PG's last (log capacity / N) eligible writes. It dates from the inline-header
record format, when on 8+3 with four copies the journal beat direct Fast EC
writes only while a stripe collected roughly ten or more writes per flush.
With records that take no PG log entry (build -55) the table ran below
journaling every write at every locality. 8+3, eleven clients at queue depth
128, IOPS as a multiple of Fast EC:

=============== =========== ====== ===== =====
working set     every write N = 10 N = 2 N = 3
=============== =========== ====== ===== =====
11 x 3 GiB      1.81        1.79
11 x 6 GiB      1.64        1.56
11 x 24 GiB     1.28        1.03   1.16  1.10
11 x 96 GiB     1.02        0.78   0.81  0.80
96 GiB, zoned   1.28        1.22   1.26  1.23
=============== =========== ====== ===== =====

So the table and its option were removed. The full-log path is rare: over
the ten journal runs behind the first two columns (296 million eligible
writes) no write waited for room or went direct because the log was full
(``pressure_events`` and ``bypass_room`` stayed 0), since reclaim keeps
ahead of the appends; and none was a whole stripe.

Replicated records
------------------

A record is stored by ``osd_ec_journal_poc_copies`` shards (0, the default,
means m + 1): the primary shard first, then shard 0 and the parity shards in
order. Under ``ec_optimizations`` these are exactly the shards that may
become primary, so a new primary always has its own copy of every record to
replay from. Each holder writes the record into its own copy of the slot in
an unlogged transaction of its own, and the client is acknowledged only
once all of them are durable. With m + 1 copies an acknowledged record
survives m failures, like the data. Records carry no log entry, so the
holders could be any shards; keeping them on the primary-eligible ones is
what makes local replay the common case.

Slots are ``ecj.<n>`` objects in the PG-local internal namespace
(``.internal_pg_local``) with the PG's own hash, the same name on every
holder. Listing (scrub, backfill, pgls) skips that namespace and client ops
addressing it are rejected. They are not temp objects: an interval change or
a restart leaves them in place.

Slot data contains only payloads. Each header is stored under an OMAP key
``r`` followed by the 16-digit hexadecimal slot offset. The header contains
magic ("ECJ6"), crc32c over header fields and the exact payload, slot offset,
object, version, object size, offset and length; nothing else is written to
the slot's OMAP, and no slot xattr is rewritten. Reusing a slot removes its
data and OMAP together before the new payload and header are written; dead
header keys need no separate trim transaction. Append and ``check_append``
share one charge per record (padded payload, header value and key,
``Journal::record_bytes``) and one rule for opening a segment, and the header
is encoded once, at the record's final slot offset.

Replay enumerates the slot's OMAP, and reads the other holders' listings
the same way (below), so nothing about a record is kept in the PG log.

Record cost
~~~~~~~~~~~

For ``rbd_data.7f1cb2a3d4e5.0000000000000001`` with an empty namespace/key,
the encoded header is about 150 bytes (the mtime added 10), its OMAP key 17
bytes, and each append's encoded OMAP update about 180 bytes. Long names and
namespaces increase these sizes.

A 4 KiB append writes 4 KiB of slot data per holder, and the holder's
transaction holds nothing else but the header and, on a rotation, the slot
reset: no PG log entry, object_info or PG info (``ec-write-journal-records.rst``
has the measured cost of the logged form it replaces, about 5.9 KiB of
RocksDB WAL per holder transaction, 1.7 KiB of it PG log and info). A 1 KiB
append still writes one data block.

A 4 KiB record is charged 4,248 bytes, about half the inline-header
format's 8 KiB, so the same ``osd_ec_journal_poc_max_bytes`` holds about 1.9
times the records, and up to about 1.9 times the live block data in the
primary's memory: live blocks take up to about 96% of ``max_bytes`` per
primary PG. Halve ``max_bytes`` to keep the inline format's record count and
memory: 256 MiB holds 63,184 4 KiB records where the inline format held
65,536 in 512 MiB. The tags add about 1.3 MiB of raw data per 10,000
retained PG-log entries and dups on each shard that logs them. OMAP adds
memtable and cache pressure; its full contents are not permanently
duplicated in the in-memory journal. RocksDB framing, WAL, SST duplication
and allocator overhead are outside the configured logical byte budget.

Benchmark both 1 KiB and 4 KiB traffic against the inline-header version,
recording commit latency, client IOPS, writes per flush, OSD RSS, device write
bytes and RocksDB WAL/flush/compaction bytes. Keep client workload and admission
settings constant, and account for the longer residence window at the same
byte budget. No performance gain is claimed from the unit-test timings.

Roll-forward kicks
~~~~~~~~~~~~~~~~~~

Every write of an optimized EC pool leaves rollback state on the shards it
writes until they learn that the write has committed everywhere. Later
writes carry that (``pg_committed_to``). When the RMW pipeline goes idle
after an op whose version is past ``can_rollback_to``, ``finish_rmw``
instead sends a dummy, transaction-empty op to every shard that still holds
rollback state. Under the 11-client QD128 load the pipeline is idle for a
moment after about a third of the writes: on 2026-10-02 the holders of the
journaled pool ran 436 transactions per second per holder role against 310
appends and 20 flush chunks, so the kicks were about a quarter of all holder
transactions. Each costs the holder a transaction with a full PG info write
(``last_update`` does not move, so the fast info key cannot be used) and a
message pair, and the next write's acknowledgement waits behind the kick's
commit in the in-order pipeline. ``osd_ec_rollforward_delay_ms`` (runtime,
default 20) defers the kick to a PG timer and drops it if a write started
meanwhile, since that write carries ``pg_committed_to`` itself; 0 restores
the immediate kick. It applies to every optimized EC pool, journaled or not.

Replay
------

An interval change drops the journal's memory: commits and flushes
registered before it never run. Every acknowledged record is on the slots of
all its holders, and the new primary is always one of them (a primary is
shard 0 or parity). A write whose record was still in flight is not
acknowledged, and its client resends it only when the primary changes
(librados), so a primary that stays requeues such writes and runs them again
in the new interval, like the ops of an aborted repop (``requeued_records``
in the PG query); a record copy a holder did commit is replayed and
superseded by the write's new record. Once every replica has activated and before any client
op runs (``PrimaryLogPG::on_activate_complete``), the primary reads its own
slots' OMAP headers, which must place their padded payloads back to back up
to the slot's size, and judges each record by its header against its copy
of the object (``ec_journal_record_state``). Only the payloads of records
still needed are read, and those are adopted in version order:

* dead if the object no longer exists, or if the object's ``ec_journal``
  attribute shows it materialized (``V <= max(base, stripes[s])`` in journal
  versions, set by a flush of the stripe or by a later write);
* deferred if this shard has to recover the object first: it is judged once
  recovery has brought the object's version and attribute up to date;
* live otherwise, and adopted into the index (per block the newest version
  wins), durable.

An adopted record stays in its slot: the journal keeps a closed segment per
slot for them, charged to the budget like appends and placed ahead of
anything this interval appends, and reuses the slot once its adopted
records have all flushed, as it does any other segment's. Adopted stripes
flush first, without waiting for age or pressure, and the PG admits new
writes into the free slots as soon as the listings and fetches below are in
(``replaying`` clears; ``replay_listed_ms`` and ``replay_admit_ms`` in the
PG query report the delays), instead of after the whole backlog. A slot
whose record is still being fetched or waits for its object's recovery is
held out of use meanwhile (``held_slots``): a new segment there would remove
the record on every holder. Stale records a returning holder kept while it
was away die by the attribute, which recovery and backfill bring it with the
object.

A bad block under a dead record therefore costs nothing. A needed payload
that does not read back intact here (a read error or a bad crc) is fetched
from the other holders, whose slots are identical, like a record missing
from the slots (below): only its object waits meanwhile
(``replay_unreadable``). Only a slot whose headers cannot be read or do not
fit its data blocks the whole PG: nothing then says which objects its
records belong to. That is reported to the cluster log, and the next
interval reads the slot again. While objects wait for a fetch or for their
recovery, the journal driver does not retry their stripes every 50 ms; the
fetch, the recovery or the next interval wakes it, and housekeeping runs
once a second.

On a PG split, every shard first waits for its queued store transactions to
apply, then copies its slot objects into the child's collection in the split
transaction, including each slot's OMAP headers. The copies use the child's PG hash but retain every byte and
record offset, so the log tags remain valid on all holders. Parent slots are
left intact. Each of the child's slots is removed first, whether the parent
has that slot or not: a PG merge leaves the source PG's slots behind under
their old hash, and a later split hands them back to the child, where their
stale data tails and headers would otherwise survive under the copy. Replay filters decoded records by the current PG's object hash
range before matching log versions or judging records. This temporarily costs
one full slot copy per child; it also works when the PG cannot flush, without
requiring an active primary or distributed writes during the split. A slot
read error aborts the split rather than committing an incomplete copy.

A record the primary's slots lack (it missed the commit: it crashed, or was
away, while the write was in flight, and came back as primary before its
replacement had flushed the record) is only on the other holders, and
nothing in the PG log says so. The primary therefore also lists the other
holders' slots: a read of each slot through the EC read pipeline with its
attributes requested, which ``handle_sub_read`` answers for a slot object
with the slot's OMAP headers. Until every listing is in, no op of the PG
runs and nothing flushes (``header_reads`` in the PG query), since a direct
write or a flush would otherwise bury a record not known yet. A listed
record the primary has not indexed is judged by its header and, unless
dead, fetched from the other holders in turn: the same slot range verbatim
(``replay_fetched``), checked (crc, object, version) and judged like local
ones. Until then its object counts as dirty, so no op of it runs first. A
record no holder returns is counted (``replay_absent``) and reported to the
cluster log. Replay retains the pending record and object barrier, reports
``replay_error = -EIO`` in the PG query, and does not complete or remove
slots. Client reads and writes of affected objects remain blocked; shutdown
drains report the error. Restore a holder with the required record and
trigger a new peering interval to retry replay. Flushes of an object also
wait until all of its pending records have been fetched or judged after
recovery, so a newer stripe marker cannot hide an older unavailable record.

A flush is an ordinary logged write of the object, built by the PG
(``PrimaryLogPG::ec_journal_flush``, an internal op like a promote): one
write per contiguous run of the stripe's live blocks, a version and a PG log
entry, the object's mtime and user version untouched. It goes through the
classic planner, so it costs what a classic write of the same blocks would:
a complete stripe is written whole without reads; an incomplete one writes
the chunks it changes plus parity, as a parity-delta or a conventional
write, whichever the planner favours. Being logged, a flush that a crash
leaves torn is rolled back by peering like any partial EC write, a shard
that misses it is recovered through its missing set, and scrub sees its
version. Flushes are issued only from the journal's timer, never inside
another op's submission. They take no object lock (client writes of the
object are in flight, and reads waiting for the flush hold a read lock) and
skip do_op's gates, so the flusher still passes over stripes that a scrub
chunk covers or whose object recovery must bring up to date first.

A flush also records what it covered in the object's ``ec_journal`` xattr
(``ECWriteJournal::Materialized``), in journal versions: ``stripes[s]`` is
the newest record version of stripe ``s`` it wrote. Every other data write
of an enrolled pool sets ``base`` to the journal version current when it
was admitted (``Journal::version_now``: every record appended so far sorts
at or below it, and any record of the object appended later sorts above
it) if the object has no live record; one that ran beside live records
sets ``stripes[s]`` to that version for the stripes it writes instead (see
"Admission"). Either way the attribute never
covers unflushed data. A record of the object with version V for stripe s
is therefore materialized exactly when ``V <= max(base, stripes[s])``, which
a replaying primary can check against the object itself. Versions of
earlier intervals sort below those of later ones, so a record a stale
primary writes late sorts below everything its successor appended or
marked. The attribute lives with the object's other attributes on shard 0
and the parity shards, so recovery and backfill carry it, and BlueStore
re-encodes it with the onode on every later write of the object.

It must therefore stay small. Each flush and each direct write beside live
records adds or raises the entries of the stripes it writes, and would leave
up to 128 entries (about 2.6 KiB) on a 4 MiB object on 8+3. So each also
raises ``base`` to just below the object's oldest live record outside the
stripes it writes (``eversion_t(epoch, version - 1)``: a PG's versions grow
across epochs, so that covers exactly the older versions), and a flush of an
object with nothing else live raises it to the flush's own version. Every
record below the object's oldest live one is materialized, or superseded by
a live record of the same block, which replay adopts as the newer version
anyway, so the rule ``V <= max(base, stripes[s])`` judges every record as
before. The generator's merge then drops the entries at or below ``base``:
they decide nothing. A flush counts its own stripe's live records as
materialized because they are the snapshot it writes; the snapshot is taken
and the flush built in one PG-lock section, so no newer record of the stripe
exists yet. Records still waiting for a replay fetch or for their object's
recovery are not in the index, but their object is neither flushed nor
written directly until they are judged.

A flush, like a direct write beside live records, submits only its marker
delta. The EC transaction generator merges it with the attribute cache in
write order, before capturing rollback attributes. Two flushes queued behind
RMW reads therefore retain both stripe markers, and rolling back the later
one restores the earlier marker.

The first version always rewrote the whole stripe: it read every data chunk
the journal did not have and wrote all k+m chunks. For a stripe holding one
block that is 7 reads and 11 writes on 8+3 against a classic write's 4 and 4,
which is why the journal ran at 0.71x classic once each flush carried only
1.5 client writes (eleven 96 GiB volumes). Planned like a client write, a
flush of one block costs exactly a classic write, and the journal's worst
case becomes a classic write plus the log append.
The normal extent cache and subwrite durable gather order physical writes.
When a flush completes, the blocks it covered become dead in the index; a
closed segment whose records are all dead and durable is released at once,
independently of the flusher.

Callbacks inside ``finish_rmw`` already run under the PG lock. The PG intrusive timer owns
PG references and schedules work without recursive completion callbacks.
An append commit wakes the driver only when it makes a flush possible that
was not possible before it (a complete stripe became durable, or the oldest
stripe of a reclaim/drain did). Flush commits also wake progress.
The timer expires incomplete stripes when ``flush_ms`` is set and, while
segments exist, wakes once a second for housekeeping: a drain requested
through the config observer (which cannot take the PG lock) or a deferred
stripe must not wait for the next client write.

**Ordering limitation:** background flush operations share the normal ordered
RMW commit queue. A slow flush can hold up ACKs for later metadata-only writes.
Thus this driver tests coalescing and aggregate read savings, not fully
independent foreground/background latency. Timer dispatch and per-stripe
flushing also impose overhead which must be reported, not hidden.
The admission FIFO is retained for bounded journal pressure and drain-before-
unsupported-operation barriers: an unsupported mutation at its head flushes
the stripes of the objects it touches, complete or not, before it proceeds,
and later writes to other objects wait behind it meanwhile.
This version fixes its ordering contracts; it does not remove cross-object
head-of-line blocking or introduce independent foreground/background
pipelines.

Experimental controls
---------------------

Startup settings (all relevant OSDs must run the modified binary):

* ``osd_ec_journal_poc_enable``: false by default.
* ``osd_ec_journal_poc_pool``: numeric test-pool ID; -1 disables enrollment.
* ``osd_ec_journal_poc_segment_bytes``: 4 MiB per-PG segment size. The
  budget holds ``max_bytes / segment_bytes`` segments; the oldest is reclaimed
  when the last one opens, so this sets how long incomplete stripes stay
  journaled and how large each reclaim is.
* ``osd_ec_journal_poc_max_bytes``: 16 MiB per-PG total encoded log budget,
  live and dead records alike. Memory holds live blocks only, up to about
  96% of this per primary PG with 4 KiB records.
* ``osd_ec_journal_poc_max_records``: 8192 records per PG in the log. Size it
  for ``max_bytes`` worth of the smallest record, or it becomes the pressure
  trigger instead of the segment count.
* ``osd_ec_journal_poc_flush_ms``: 100 ms maximum age of an incomplete
  stripe; 0 leaves incomplete stripes to pressure and drain only.

The pool must use ``allow_ec_overwrites`` and ``allow_ec_optimizations`` and a
4+3, 8+3 or 12+3 profile with 4096-byte chunks. Prefill the test data with journaling
off or draining, then stop clients before starting the measured phase.
Do not enable on any existing production pool. Budgets are per PG, not per OSD.
Startup enrollment settings are read once per backend. Unsupported geometry
or invalid capacity limits log an error and refuse enrollment rather than
throwing/asserting. Size options are read as ``Option::size_t``. A record too
large for a configured segment falls back after draining older records.

``osd_ec_journal_poc_drain`` is a runtime boolean: stop writers and set it on
all participating OSDs. It prevents further enrollment, flushes every dirty
stripe and removes the dead segments; idle PGs pick it up within the
one-second housekeeping wake. Wait for PG-query ``ec_write_journal_poc``
counters ``bytes``, ``records``, ``pending_admissions`` and
``flushes_in_flight`` to reach zero
before ending the experiment. A client read of journaled data is held until
the stripes it overlaps have flushed (``read_barrier_events``,
``reads_waiting``); without that barrier a filesystem on a journaled pool
read its own recent metadata from the shards, stale (a volume formatted from
one host could not be mounted from another until the journal drained).
Internal reads (recovery, backfill, scrub) are not covered: they still require
a clean journal. PrimaryLogPG completes a PG's async reads strictly in the
order it issued them, so once one read waits every later read of the PG
queues behind it, and reads leave only from the front. The first version let
a read that did not overlap journaled data overtake a waiting one: the PG
asserted on the out-of-order completion, and because the prototype cannot
replay, the resulting interval changes crashed every OSD that held journaled
data and lost those acknowledged writes.

The query also reports ``payload_bytes`` and ``header_bytes`` (logical
encoded headers and keys), ``segments``, ``live_blocks``/``live_bytes`` (index
memory), ``superseded_blocks``, ``pressure``, ``reclaiming`` (a reclaim
bound is set), ``admitted``, ``acked``,
``full_stripes``, ``partial_stripes``, the reason for each incomplete flush
(``pressure_flushes``, ``aged_flushes``, ``drain_flushes``,
``barrier_flushes``; until the object-scoped barrier, barrier flushes were
counted as ``pressure_flushes``), ``barrier_events`` (unsupported mutations
that had to wait for journaled data), ``barrier_objects`` (objects such a
mutation is waiting for now),
``read_barrier_events`` (client reads that waited for a flush),
``reads_waiting`` (reads waiting now),
``deferred_flushes`` (candidates passed over for a scrub or degraded object),
``flushed_logical_bytes`` and ``pressure_events``. Measure device reads
separately. Journal-full requests that the journal has to take wait in do_op
without ACK or version, under the OSD's existing client throttles.
``pressure_events`` counts those waits, including a requeued request's
retries. The admission counters are described under "Admission".

A flush is deferred (50 ms retries) while the scrubber holds the object's
range or the object is degraded, backfilling or unreadable, mirroring
``do_op``'s gating for client writes, which an internal op skips.
``osd_ec_journal_poc_drain`` is tracked through a config observer rather than
looked up per pump; the observer cannot take the PG lock, so it does not wake
the driver itself.

Graceful OSD shutdown (including the fast-shutdown path) first stops local
enrollment and requests drains while workers and cluster messaging are live,
before notifying the monitor or entering ``STATE_STOPPING``. The shutdown
thread waits outside PG/OSD locks, at most 30 seconds; timeout/cancellation
logs a warning and shuts down anyway: the next primaries replay whatever is
left, so draining first only saves them that work.

A PG reset with live records is replayed as described above; the slot
objects stay on disk until a later segment in the same slot removes them. No deployments or changes to the historical fleet are made by
this patch.

Validation
----------

Normal core build target: ``unittest_ec_write_journal``. It links the small journal
component rather than the entire OSD backend. The tests cover all three geometries,
both write sizes, exact ObjectStore record encoding, commit-before-flush,
eager complete stripes, age/pressure/drain policies for incomplete ones,
strict oldest-first order, supersede accounting and cross-segment coalescing,
segment death by flush or overwrite, writes arriving while their stripe
flushes, capacity, skipped candidates, reset handling, failure handling and a
randomized reference check with interleaved flushes and reclaims.
Split regressions cover unchanged slot offsets and OMAP headers, shard and PG
hashes, ownership filtering, rejected partial tails, read errors and the
removal of every child slot before the copy. Replay regressions cover failed
fetches retaining object barriers, unreadable slots blocking untagged
objects, and prevention of premature flushes. Record-format tests check
one-block 4 KiB writes, no slot xattr operations, byte budgets, checksums, missing, misplaced and short headers,
headers of records that open later segments, payloads of several blocks
(4 KiB multiples or not) located by their headers alone, remote-fetch
header/tag consistency and which op_returns items count as log tags.

``unittest_ec_journal_flush`` additionally exercises the actual EC planner and
shard extent mapping for all three geometries: full/partial pages, complete stripes,
nonzero offsets, unaligned 4 KiB writes, sparse fragments, EOF padding,
metadata-only foreground plans, zero-filled absent base ranges, and the
admission-time size check against the planner's view of the object. It runs
the real transaction generator on a journaled (data-less) write and checks
that only shard 0 and the parity shards receive a transaction, that the
other data shards are recorded in ``shard_versions``, and that the first
write of an interval still reaches every shard. The backend's use of that
contract is compile-checked only.
Its admission tests use the same option extraction, eligibility, FIFO barrier
and OBC snapshot helpers as the backend. Generation tests fold real journal
records and run ``plan_flush``/``assemble_flush`` against nonzero base data.
Queued-flush tests run the real transaction generator and check that stripe
markers accumulate without losing the existing base or other stripes, and
that a flush's raised base drops the entries it covers (an object with
nothing else live ends with an empty map). A
direct-write test runs it on a real partial write of one stripe beside live
records (through a stub plugin that writes zero parity) and checks that the
marker delta merges the same way and leaves a live record of another stripe
uncovered. Journal tests cover ``oldest_version`` (stripes skipped, blocks
superseded, other objects) and the marker merge (pruning, ``just_below``
across epochs). The admission decision itself (``journal_admit``) is
compile-checked only.
The logical reference model (``holes``/``assemble``) lives in the unit tests,
not in ``libosd``; the OSD only has the shard planner/assembly.

``src/script/test-ec-write-journal-poc.sh`` offers a focused build with an
existing compatible Ceph build. Set ``CEPH_DEPS_SOURCE``, ``CEPH_DEPS_BUILD``,
``CXX`` and, if necessary, ``BOOST_ROOT``. Run the script with bash; pass gtest
arguments as script arguments. ``EC_JOURNAL_SANITIZE=1`` enables ASan/UBSan for
the new code and tests (the reused Ceph shared library is not instrumented).
The dependency checkout is read-only; artifacts go under ``build-journal-poc``
or ``EC_JOURNAL_TEST_BUILD``. This is not a replacement for a fresh full build.
``EC_JOURNAL_FLUSH_TESTS=1`` includes the EC planner/assembly suite;
``EC_JOURNAL_BACKEND_CHECK=1`` also compile-checks the actual backend, common
planner, transaction generator, journal core, PrimaryLogPG integration and OSD
shutdown unit with warnings treated as errors. It requires
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
  Exercise PG splits with live journal records and failover before and after
  the split commits, plus missing-record replay with every holder unavailable.
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
   healthy write/drain path is demonstrated. Reads of journaled data wait for
   a flush; there is no overlay.

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