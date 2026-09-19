// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <vector>

#include "common/hobject.h"
#include "os/Transaction.h"

namespace ECWriteJournal {

// Experimental, primary-local journal building block. All methods must run
// under the owning PG lock. This class does NOT submit transactions, acknowledge
// client requests, or implement replay. See doc/dev/ec-write-journal-poc.rst.
inline constexpr uint64_t block_size = 1024;

struct Record {
  uint64_t sequence = 0;
  hobject_t object;
  eversion_t version;
  uint64_t object_size = 0;
  uint64_t offset = 0;
  ceph::bufferlist data;

  void encode(ceph::bufferlist& out) const;
  void decode(ceph::bufferlist::const_iterator& in);
};
WRITE_CLASS_ENCODER(Record)

struct Stripe {
  hobject_t object;
  uint64_t object_size = 0;
  uint64_t offset = 0;
  uint64_t width = 0;
  uint64_t valid_bytes = 0; // logical bytes before EOF, at most width
  // Relative, 1 KiB aligned offsets. Bufferlists share immutable record data.
  std::map<uint64_t, ceph::bufferlist> blocks;

  uint64_t dirty_bytes() const { return blocks.size() * block_size; }
  bool full() const { return dirty_bytes() == valid_bytes; }
  // Logical object ranges that still need base data (not rounded device reads).
  std::vector<std::pair<uint64_t, uint64_t>> holes() const;
  // A complete stripe needs no base. Otherwise base must contain valid_bytes
  // from the pre-existing stripe. Only the known range beyond EOF is zeroed;
  // encoding padding must not be used to extend the logical object size.
  ceph::bufferlist assemble(const ceph::bufferlist* base = nullptr) const;
};

struct Limits {
  uint64_t stripe_width = 8 * 4096;
  uint64_t segment_bytes = 4 * 1024 * 1024;
  uint64_t max_bytes = 16 * 1024 * 1024;
  uint64_t max_records = 8192;
  uint64_t max_segments = 4;
};

struct Ticket {
  uint64_t segment = 0;
  uint64_t sequence = 0;
};

struct Flush {
  uint64_t segment = 0;
  uint64_t records = 0;
  uint64_t payload_bytes = 0;
  uint64_t journal_bytes = 0;
  std::vector<Stripe> stripes;
};

class Journal {
 public:
  // prefix must be a collision-free, PG-local internal object name, unique to
  // this experiment's lifetime. No reopening existing journal segments yet.
  Journal(coll_t collection, ghobject_t prefix, Limits limits);

  // Prepare an append to an ObjectStore transaction. Exact 1 KiB/4 KiB writes,
  // aligned to 1 KiB, within a prefilled object and one stripe are supported.
  // On success the caller MUST submit t. Append does NOT mean durable/ACKable.
  // -EAGAIN means drain/backpressure; no state or transaction change on error.
  int append(const hobject_t& object, eversion_t version,
             uint64_t object_size, uint64_t offset,
             const ceph::bufferlist& data, ceph::os::Transaction& t,
             Ticket* ticket);

  // Call ONLY from a successfully submitted append's on_commit callback,
  // wrapped in PGBackend::Listener::bless_context() by the backend owner.
  // Append errors latch the journal closed; never ACK them or silently bypass.
  int committed(Ticket ticket, int result);

  // Timer, pressure, or explicit drain seals the open segment. Empty is a no-op.
  void seal();
  // At most one segment can flush; sealed segments drain in append order. A
  // segment is not exposed until ALL of its append transactions are durable.
  std::optional<Flush> begin_flush();
  // Call only after ALL EC writes covering this Flush have durably completed.
  // On success prepares removal of the segment in t; submit that transaction,
  // then call trimmed() from its blessed on_commit callback. On error retains
  // all records and makes the segment available for a retry.
  int finish_flush(uint64_t segment, int result, ceph::os::Transaction& t);
  int trimmed(uint64_t segment, int result);

  ghobject_t segment_object(uint64_t segment) const;
  uint64_t bytes() const { return used_bytes; }
  uint64_t records() const { return used_records; }
  bool empty() const { return segments.empty(); }
  int error() const { return failed; }

 private:
  enum class State { open, sealed, flushing, trimming };
  struct Pending {
    Record record;
    bool durable = false;
  };
  struct Segment {
    uint64_t id;
    State state = State::open;
    uint64_t bytes = 0;
    uint64_t durable_records = 0;
    std::map<uint64_t, Pending> entries;

    explicit Segment(uint64_t id) : id(id) {}
  };

  coll_t collection;
  ghobject_t prefix;
  Limits limits;
  std::deque<Segment> segments;
  uint64_t next_segment = 1;
  uint64_t next_sequence = 1;
  uint64_t used_bytes = 0;
  uint64_t used_records = 0;
  int failed = 0;
};

} // namespace ECWriteJournal