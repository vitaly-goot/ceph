// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include "ECWriteJournal.h"
#include "ECTransaction.h"
#include "common/options.h"

namespace ECWriteJournal {

inline bool supported_geometry(unsigned k, unsigned m, uint64_t chunk_size)
{
  return (k == 4 || k == 8 || k == 12) && m == 3 && chunk_size == 4096;
}

// Keep config extraction shared with tests: size and uint are distinct option
// alternatives even though both convert to uint64_t.
template<typename Config>
Limits read_limits(const Config& conf, uint64_t stripe_width)
{
  Limits limits;
  limits.stripe_width = stripe_width;
  limits.segment_bytes = conf.template get_val<Option::size_t>(
    "osd_ec_journal_poc_segment_bytes");
  limits.max_bytes = conf.template get_val<Option::size_t>(
    "osd_ec_journal_poc_max_bytes");
  limits.max_records = conf.template get_val<uint64_t>(
    "osd_ec_journal_poc_max_records");
  limits.max_segments = limits.segment_bytes
    ? std::max(uint64_t(1), limits.max_bytes / limits.segment_bytes) : 0;
  return limits;
}

bool eligible_overwrite(const hobject_t& object,
  const PGTransaction::ObjectOperation& update, uint64_t old_size,
  uint64_t stripe_width);

// Plan the reads for a conventional (not parity-delta) complete-stripe flush.
// Dirty 1K fragments use the existing planner's partial-page read handling.
// All shard chunks are written; object_info.size is never changed by a flush.
ECTransaction::WritePlanObj plan_flush(
  const Stripe& stripe, const ECUtil::stripe_info_t& sinfo,
  const shard_id_set& shards);

// Overlay journal data onto the planner's read result and supply only known
// EOF zeros. Missing live base bytes are an error, not implicit zeros.
ECUtil::shard_extent_map_t assemble_flush(
  const Stripe& stripe, const ECUtil::stripe_info_t& sinfo,
  const ECUtil::shard_extent_map_t& base);

} // namespace ECWriteJournal