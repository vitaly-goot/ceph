Unlogged journal records: proposal
==================================

Status: proposal, 2026-10-02. Measurements referenced here were taken on the
test cluster under the build -50 chain (11 clients, 4 KiB randwrite, QD128,
pool ``ec83_stk`` 8+3 with 512 PGs) and are kept with the handoff results.

The target
----------

The EC write journal (``ec-write-journal-poc.rst``) must reach 2x Fast EC
IOPS on the saturation benchmark at good locality (11 x 3 GiB volumes) and
never fall below Fast EC at bad locality, with the pool's durability (an
acknowledged write survives m = 3 OSD failures) and device writes no worse
than Fast EC. Build -49/-50 stand at 1.20-1.25x and 0.81x.

Where the journal loses its 2x
------------------------------

The journal's median write is already 1.8x faster than Fast EC's; its tail
is not. At a fixed queue depth the throughput is the queue depth over the
*mean* latency, and the mean is set by the tail:

=================== ======= ======= ===== ===== =====
run (3 GiB, -50)    IOPS    mean ms p50   p90   p99
=================== ======= ======= ===== ===== =====
Fast EC             134,219 10.5    6.7   19.9  68.5
journal, 4 copies   160,744 8.75    3.7   22.5  65.7
journal, 1 copy -46 215,560 6.5     3.4   14.0  53.0
=================== ======= ======= ===== ===== =====

With Fast EC's own mean-to-median ratio (1.57) the journal's median would
give a mean of 5.8 ms, 243k IOPS, 1.8x. The one-copy row shows the tail is
not mainly the holders' copies.

What the holders pay per client write (osd.5, 30 s, 6 GiB journal load):

* Transactions per OSD and second fit ``1,917 + 436 x holder roles``
  (correlation 1.00 over the 55 OSDs; holder roles are shard 0 and the
  three parity shards, 21-53 per OSD). Per holder role and second: 310
  record appends, 20 flush chunks, and about 106 *roll-forward kicks*: the
  dummy op ``ECCommon::RMWPipeline::finish_rmw`` sends whenever the extent
  cache goes idle, one per three writes under this load. A kick costs the
  holder a transaction with a full ``_info`` write (``last_update`` does
  not move, so the fast-info key cannot be used), a message pair, and the
  next write's acknowledgement waits behind it in the in-order pipeline.
  Fast EC's parity shards pay the same. ``osd_ec_rollforward_delay_ms``
  (commit 6fb150e7ae7) debounces it.
* ``bstore_kv_sync`` is 0.90 busy on average and 0.97 on the busiest
  holders: 20 us of RocksDB submit per transaction (about seven keys, 5.9
  KiB of WAL, 1.7 KiB of it PG log and info OMAP) plus 107 us of WAL fsync
  per batch of 4.65 transactions. Transactions wait 0.8 ms in the kv queue
  and 0.8 ms in the batch. The NVMe is not the limit (``w_await`` 0.1 ms,
  no spike above 1.8 ms in 60 s); RocksDB logs no write stalls.
* The three messenger workers are 68-81% busy; the op worker threads
  15-45%. Sub-writes spend about as long waiting for the PG in the op queue
  as in BlueStore.
* A journaled write needs no reads but queues in the extent cache's
  per-PG FIFO (``ECExtentCache::cache_maybe_ready``) behind any flush of an
  incomplete stripe that waits for reads, and in ``waiting_commit`` behind
  any flush's 11-shard commit. Both are required for logged ops: sub-writes
  must leave in version order and ``pg_committed_to`` must stay monotonic.
  The 20 slowest ops on osd.7 were ten writes stuck 690 ms before their
  own sub-write started and ten stuck 570 ms waiting for one holder.

The levers, then, are the holder transaction (its count, its keys and its
bytes), the hops a journaled write takes through op queues, and the
pipeline it shares with flushes. Coalescing cannot do it: at 3 GiB a stripe
already completes before it flushes.

Design: records without log entries
-----------------------------------

A journaled write stops being a logged partial write. It becomes a record
only: a 4 KiB payload in the slot object plus a 164-byte OMAP header, on
``copies`` (m + 1 = 4) shards, with no PG log entry, no object_info update
and no PG info write anywhere. The holder transaction shrinks from about
seven keys and 5.9 KiB of WAL to three keys and about 1.6 KiB (slot onode
and extent shard, header, statfs). The flush stays what it is: an ordinary
logged write of the stripe, through the RMW pipeline.

Primary path
~~~~~~~~~~~~

``do_op`` admits the write as today (``journal_admit``) and then hands it
to the backend without an ``OpContext``: no ``execute_ctx``, no version, no
log entry. The backend places the record (``Journal::append``), builds one
transaction per target shard (slot write, header, slot reset when the
segment rotates) and sends them: the local one straight to the store, the
others as ``ECSubWrite`` with a new ``unlogged`` flag, no entries and
version 0. Replies are routed by tid to a journal in-flight table, not the
pipeline. When all copies have committed, the record is marked durable and
the client is acknowledged, in order per object (a per-object FIFO of
in-flight records), never waiting for flushes or other objects. Stats go
through ``apply_stats``. The reply carries the object's current versions.

Holder path
~~~~~~~~~~~

``handle_sub_write`` with ``unlogged`` skips ``log_operation``, the stats
update and ``op_applied``: it queues the record transaction and replies on
commit as now. The message still passes the op queue and the PG lock, which
keeps the interval check; moving it to fast dispatch is a later step.

Record versions
~~~~~~~~~~~~~~~

Records no longer have PG versions. Each carries a journal version
``jver = (E, seq)``: E is the interval's start epoch, seq the primary's
append sequence. ``eversion_t`` holds it, so ordering, ``just_below`` and
the ``Materialized`` marker work unchanged, in a new version space. A new
interval has a larger E than everything before it, and a record a stale
primary writes after the next primary replayed sorts below anything that
primary writes.

The marker becomes ``{base_j, stripes[s]}`` in jver space, with the same
rule: a record ``(jver, s)`` is dead iff the object does not exist or
``jver <= max(base_j, stripes[s])``. A flush of stripe s with snapshot
maximum J sets ``stripes[s] = J`` and raises ``base_j`` to just below the
object's oldest live record on other stripes, or to J when nothing else is
live. A direct write (admission control bypass) of stripe s' sets
``stripes[s'] = now`` where now is the primary's current sequence; any
other data write ran because the object had no live record, and sets
``base_j = now``. The divergence rule (``V <= OI.version``) goes away with
the log entries: an un-acknowledged record found at replay is applied if
nothing covers it, which is allowed for an un-acknowledged write and
idempotent against the client's resend (whole-block overwrites; the newest
jver of a block wins).

Placement on all shards
~~~~~~~~~~~~~~~~~~~~~~~

Without log entries the copies need not sit on the primary-eligible
shards. Record i goes to shards ``(i + j) mod n`` for j < copies over the
acting set, so every OSD carries 4/11 of each PG's records instead of some
OSDs carrying 1/1 of 53 PGs and others of 21. Each shard's slot is sparse:
offsets stay global, so the slot size, the header keys and the fetch
locators are identical on every shard; a shard removes its copy of a slot
before its first write into a rotated segment (a per-segment set of reset
shards). Admission still requires a complete acting set with no backfill
target, as now.

Replay
~~~~~~

The new primary reads the slot headers of every acting shard, not only its
own (``ECSubRead`` gains an OMAP-read request; headers are 164 bytes, at
most about 4 MiB per shard per PG), takes the union, judges each record by
the marker rule above against its local copy of the object (deferring
objects it must recover first, as now), and reads the live payloads from
any shard whose listing has them, local first, through the existing fetch
path. Then it flushes everything and reopens admission. The PG log tag and
the log-driven fetch are retired.

What stays unchanged
~~~~~~~~~~~~~~~~~~~~

Admission control and the recency table; the barrier for non-candidate
ops and the read barrier; logged flushes with their rollback and
recovery semantics; slot copy on split; the drain and slot removal; the
primary's live-block index and its memory bound; scrub, backfill and
recovery, which see shards unchanged until a flush.

Semantics that change
~~~~~~~~~~~~~~~~~~~~~

* Journaled writes are not in the PG log: a client retry after a failover
  is re-executed rather than answered from the dups. For whole-block
  overwrites of an RBD image that is idempotent.
* The object's mtime and user_version move at the flush, not at the write.
  The record header carries the client's mtime so the flush can apply it.
* Completion order is kept per object (acks of one object's journaled
  writes are in order); writes to different objects complete independently,
  as they do on a block device.

Model
-----

Per client write at 3 GiB (21 writes per flushed stripe, from the -50
counters), holder side:

======================== =========== ================ ============
                         transactions WAL KiB (approx) data KiB
======================== =========== ================ ============
today, 4 copies          4 + 0.52 + 1.35 kicks 4 x 5.9 + 1.6 + 2.7 = 28 18.1
with 6fb150e7ae7         4 + 0.52    25               18.1
records only             4 + 0.52    4 x 1.6 + 1.6 = 8 18.1
Fast EC (+ kicks)        4 + 1.35    30.6             16.0
======================== =========== ================ ============

Device writes per client write drop from 69 KiB to about 30 (data 18, WAL
8, SST at the WAL's share), against Fast EC's 74: wear at good locality is
0.4x Fast EC instead of 0.93x. At bad locality the direct path is Fast EC's
and the journaled share is admission-controlled, so the bound is 1.0x.

Per-OSD kv_sync at 160k IOPS: 18.2k transactions per second becomes 13.3k
(no kicks) at about 9 us of submit each instead of 20 (three keys, a
quarter of the bytes), 0.45 busy instead of 0.90. At 320k IOPS: 26.6k per
second, 0.24 of submit plus roughly 0.6 of fsync at larger batches, 0.84
busy. The messenger needs 7 message-events per write instead of 9.7, so 81k
per second per OSD at 320k against 67k today at 160k; ``ms_async_op_threads``
4-5 instead of 3 covers it for both modes.

Latency: the median loses the holder's log work (0.3 ms), the primary's
``execute_ctx`` (0.25 of the 0.35 ms prepare), and one dummy per three
writes ahead in the commit order; the tail loses the two head-of-line
queues behind flushes and the kicks, and the busiest holders run at the
mean load instead of 1.4x it. A median near 2.5 ms with Fast EC's tail
ratio is a mean of 4.0-4.5 ms, 310-350k IOPS, 2.3-2.6x; half of that gain
is 2.0x. The experiment decides; the single-volume QD16 run (0.82 ms per
write, -46) shows the floor is far below.

Memory: unchanged, the index is the same. Disk: the same slots, spread
over 11 shards instead of 4. Durability: four copies on four shards, hence
four hosts under the host failure domain, acknowledged after all four
commit: survives three failures, as the data does.

Plan
----

1. Measure 6fb150e7ae7 (build -53): holder transactions and full-info
   writes per second, Fast EC and journal, 3 and 24 GiB.
2. Records only on the four primary-eligible holders, replay by union of
   their headers (ECSubRead OMAP read, jver marker, unlogged sub-write,
   journal in-flight table, per-object acks). Measure.
3. Placement over all acting shards (sparse slots, per-segment reset set,
   fetch from any listing shard). Measure.
4. Fast dispatch of the record sub-write if the holder's op-queue hop
   still shows in the tail.

Each step passes the focused tests, Part A, Part C and the corruption test
before its numbers count.
