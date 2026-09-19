// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include "ECWriteJournal.h"
#include "ECTransaction.h"

namespace ECWriteJournal {

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