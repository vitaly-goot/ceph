// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// SPDX-License-Identifier: LGPL-2.1-or-later
#include "ECJournalFlush.h"

namespace ECWriteJournal {

ECTransaction::WritePlanObj plan_flush(
  const Stripe& stripe, const ECUtil::stripe_info_t& sinfo,
  const shard_id_set& shards)
{
  ceph_assert(stripe.width == sinfo.get_k() * sinfo.get_chunk_size());
  ceph_assert(stripe.offset % stripe.width == 0);
  ceph_assert(stripe.object_size >= stripe.offset + stripe.valid_bytes);
  PGTransaction::ObjectOperation updates;
  for (const auto& [off, data] : stripe.blocks) {
    updates.buffer_updates.insert(stripe.offset + off, data.length(),
      PGTransaction::ObjectOperation::BufferUpdate::Write{data, 0});
  }
  object_info_t oi;
  oi.size = stripe.object_size;
  ECTransaction::WritePlanObj plan(stripe.object, updates, sinfo, shards,
    shards, false, oi.size, oi, std::nullopt, 1 /* never PDW */);
  plan.will_write.clear();
  sinfo.ro_range_to_shard_extent_set_with_parity(
    stripe.offset, stripe.width, plan.will_write);
  return plan;
}

ECUtil::shard_extent_map_t assemble_flush(
  const Stripe& stripe, const ECUtil::stripe_info_t& sinfo,
  const ECUtil::shard_extent_map_t& base)
{
  ECUtil::shard_extent_map_t data = base;
  for (const auto& [off, bytes] : stripe.blocks) {
    ceph::bufferlist block = bytes;
    sinfo.ro_range_to_shard_extent_map(
      stripe.offset + off, block.length(), block, data);
  }
  if (stripe.valid_bytes < stripe.width) {
    ceph::bufferlist zeros;
    zeros.append_zero(stripe.width - stripe.valid_bytes);
    sinfo.ro_range_to_shard_extent_map(
      stripe.offset + stripe.valid_bytes, zeros.length(), zeros, data);
  }
  const uint64_t chunk_off = stripe.offset / sinfo.get_k();
  for (auto shard : sinfo.get_data_shards()) {
    ceph::bufferlist chunk;
    // An absent shard would make get_buffer() throw; explicitly diagnose it.
    ceph_assert(data.contains_shard(shard));
    data.get_buffer(shard, chunk_off, sinfo.get_chunk_size(), chunk);
    ceph_assert(chunk.length() == sinfo.get_chunk_size());
  }
  return data;
}

} // namespace ECWriteJournal