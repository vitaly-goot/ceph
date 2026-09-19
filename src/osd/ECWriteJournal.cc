// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// SPDX-License-Identifier: LGPL-2.1-or-later
#include "ECWriteJournal.h"

#include <algorithm>
#include <cerrno>
#include <limits>
#include <stdexcept>
#include <utility>

namespace ECWriteJournal {

void Record::encode(ceph::bufferlist& out) const
{
  ENCODE_START(1, 1, out);
  encode(sequence, out);
  encode(object, out);
  encode(version, out);
  encode(object_size, out);
  encode(offset, out);
  encode(data, out);
  ENCODE_FINISH(out);
}

void Record::decode(ceph::bufferlist::const_iterator& in)
{
  DECODE_START(1, in);
  decode(sequence, in);
  decode(object, in);
  decode(version, in);
  decode(object_size, in);
  decode(offset, in);
  decode(data, in);
  DECODE_FINISH(in);
}

std::vector<std::pair<uint64_t, uint64_t>> Stripe::holes() const
{
  std::vector<std::pair<uint64_t, uint64_t>> result;
  uint64_t pos = 0;
  for (const auto& [off, data] : blocks) {
    if (pos < off) {
      result.emplace_back(offset + pos, off - pos);
    }
    pos = off + block_size;
  }
  if (pos < valid_bytes) {
    result.emplace_back(offset + pos, valid_bytes - pos);
  }
  return result;
}

ceph::bufferlist Stripe::assemble(const ceph::bufferlist* base) const
{
  if (!full() && (!base || base->length() != valid_bytes)) {
    throw std::invalid_argument("incomplete EC journal stripe requires base data");
  }
  ceph::bufferlist out;
  for (uint64_t off = 0; off < valid_bytes; off += block_size) {
    auto i = blocks.find(off);
    if (i != blocks.end()) {
      out.append(i->second);
    } else {
      ceph::bufferlist original;
      original.substr_of(*base, off, std::min(block_size, valid_bytes - off));
      out.append(original);
    }
  }
  out.append_zero(width - valid_bytes);
  return out;
}

Journal::Journal(coll_t collection, ghobject_t prefix, Limits limits)
  : collection(std::move(collection)), prefix(std::move(prefix)), limits(limits)
{
  if (!limits.stripe_width || limits.stripe_width % 4096 ||
      !limits.segment_bytes || limits.segment_bytes > limits.max_bytes ||
      !limits.max_records || !limits.max_segments) {
    throw std::invalid_argument("invalid EC journal geometry or capacity");
  }
}

ghobject_t Journal::segment_object(uint64_t segment) const
{
  auto object = prefix;
  object.hobj.oid.name += "." + std::to_string(segment);
  return object;
}

int Journal::append(const hobject_t& object, eversion_t version,
                    uint64_t object_size, uint64_t offset,
                    const ceph::bufferlist& data, ceph::os::Transaction& t,
                    Ticket* ticket)
{
  if (failed) {
    return failed;
  }
  const uint64_t len = data.length();
  if (!ticket || (len != block_size && len != 4096) || offset % block_size ||
      offset > object_size || len > object_size - offset ||
      len > limits.stripe_width - offset % limits.stripe_width) {
    return -EINVAL;
  }
  if (next_sequence == std::numeric_limits<uint64_t>::max() ||
      next_segment == std::numeric_limits<uint64_t>::max()) {
    return -EOVERFLOW;
  }

  Record record{next_sequence, object, version, object_size, offset, data};
  ceph::bufferlist encoded;
  encode(record, encoded);
  const uint64_t bytes = encoded.length();
  if (bytes > limits.segment_bytes) {
    return -E2BIG;
  }
  if (used_records == limits.max_records || bytes > limits.max_bytes - used_bytes) {
    return -EAGAIN;
  }
  bool rotate = segments.empty() || segments.back().state != State::open ||
    bytes > limits.segment_bytes - segments.back().bytes;
  if (rotate && segments.size() == limits.max_segments) {
    return -EAGAIN;
  }
  if (rotate) {
    seal();
    segments.push_back(Segment{next_segment++});
  }
  auto& segment = segments.back();
  t.write(collection, segment_object(segment.id), segment.bytes, bytes, encoded);
  segment.entries.emplace(next_sequence, Pending{std::move(record)});
  *ticket = Ticket{segment.id, next_sequence++};
  segment.bytes += bytes;
  used_bytes += bytes;
  ++used_records;
  return 0;
}

int Journal::committed(Ticket ticket, int result)
{
  for (auto& segment : segments) {
    if (segment.id != ticket.segment) {
      continue;
    }
    auto i = segment.entries.find(ticket.sequence);
    if (i == segment.entries.end()) {
      return -ENOENT;
    }
    if (result < 0) {
      if (!failed) {
        failed = result;
      }
      return result;
    }
    if (i->second.durable) {
      return -EALREADY;
    }
    i->second.durable = true;
    ++segment.durable_records;
    return failed;
  }
  return -ENOENT;
}

void Journal::seal()
{
  if (!segments.empty() && segments.back().state == State::open) {
    segments.back().state = State::sealed;
  }
}

std::optional<Flush> Journal::begin_flush()
{
  if (failed || segments.empty()) {
    return std::nullopt;
  }
  auto& segment = segments.front();
  if (segment.state != State::sealed ||
      segment.durable_records != segment.entries.size()) {
    return std::nullopt;
  }
  Flush flush;
  flush.segment = segment.id;
  flush.records = segment.entries.size();
  flush.journal_bytes = segment.bytes;
  std::map<std::pair<hobject_t, uint64_t>, Stripe> stripes;
  // Fold by append sequence, NOT by callback arrival order. Latest write wins.
  for (const auto& [sequence, pending] : segment.entries) {
    const auto& record = pending.record;
    const uint64_t start = record.offset / limits.stripe_width * limits.stripe_width;
    auto [i, inserted] = stripes.try_emplace(
      std::make_pair(record.object, start));
    auto& stripe = i->second;
    if (inserted) {
      stripe.object = record.object;
      stripe.offset = start;
      stripe.width = limits.stripe_width;
      stripe.valid_bytes = std::min(limits.stripe_width, record.object_size - start);
    }
    for (uint64_t off = 0; off < record.data.length(); off += block_size) {
      ceph::bufferlist block;
      block.substr_of(record.data, off, block_size);
      stripe.blocks[record.offset - start + off] = std::move(block);
    }
    flush.payload_bytes += record.data.length();
  }
  for (auto& [key, stripe] : stripes) {
    flush.stripes.push_back(std::move(stripe));
  }
  segment.state = State::flushing;
  return flush;
}

int Journal::finish_flush(uint64_t id, int result, ceph::os::Transaction& t)
{
  if (segments.empty() || segments.front().id != id) {
    return -ENOENT;
  }
  auto& segment = segments.front();
  if (segment.state != State::flushing) {
    return -EINVAL;
  }
  if (result < 0) {
    segment.state = State::sealed;
    return result;
  }
  t.remove(collection, segment_object(id));
  segment.state = State::trimming;
  return 0;
}

int Journal::trimmed(uint64_t id, int result)
{
  if (segments.empty() || segments.front().id != id) {
    return -ENOENT;
  }
  auto& segment = segments.front();
  if (segment.state != State::trimming) {
    return -EINVAL;
  }
  if (result < 0) {
    // Base writes are already durable. Retry removal, never free the budget.
    segment.state = State::flushing;
    return result;
  }
  used_bytes -= segment.bytes;
  used_records -= segment.entries.size();
  segments.pop_front();
  return 0;
}

} // namespace ECWriteJournal