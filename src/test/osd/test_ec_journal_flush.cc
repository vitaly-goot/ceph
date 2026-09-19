// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// SPDX-License-Identifier: LGPL-2.1-or-later
#include <gtest/gtest.h>
#include "osd/ECJournalFlush.h"

namespace {
class JournalFlushTest : public testing::TestWithParam<unsigned> {
 protected:
  uint64_t width() const { return GetParam() * 4096; }
  ECWriteJournal::Stripe stripe(uint64_t offset = 0, uint64_t tail = 0) {
    ECWriteJournal::Stripe s;
    s.object = hobject_t(object_t("data"), "", CEPH_NOSNAP, 0, 1, "");
    s.offset = offset;
    s.width = width();
    s.object_size = tail ? offset + tail : width() * 4;
    s.valid_bytes = tail ? tail : width();
    return s;
  }
  void write(ECWriteJournal::Stripe& stripe, uint64_t offset, uint64_t length) {
    for (uint64_t off = offset; off < offset + length; off += 1024) {
      ceph::bufferlist b;
      b.append(std::string(1024, 'x'));
      stripe.blocks[off] = b;
    }
  }
  uint64_t reads(const ECTransaction::WritePlanObj& plan) {
    uint64_t bytes = 0;
    if (plan.to_read) {
      for (const auto& [shard, extents] : *plan.to_read) {
        bytes += extents.size();
      }
    }
    return bytes;
  }
  void verify(ECWriteJournal::Stripe stripe, uint64_t expected_reads) {
    pg_pool_t pool;
    pool.set_flag(pg_pool_t::FLAG_EC_OPTIMIZATIONS);
    ECUtil::stripe_info_t sinfo(GetParam(), 3, width(), &pool);
    shard_id_set shards;
    shards.insert_range(shard_id_t(0), GetParam() + 3);
    auto plan = ECWriteJournal::plan_flush(stripe, sinfo, shards);
    EXPECT_FALSE(plan.do_parity_delta_write);
    EXPECT_EQ(stripe.object_size, plan.orig_size);
    EXPECT_EQ(stripe.object_size, plan.projected_size);
    EXPECT_EQ(expected_reads, reads(plan));
    EXPECT_EQ(GetParam() + 3, plan.will_write.shard_count());

    // Emulate exactly the planner-requested base reads, including page padding.
    // Missing live ranges must NOT be silently supplied by assemble_flush.
    ECUtil::shard_extent_map_t base(&sinfo);
    if (plan.to_read) {
      for (const auto& [shard, extents] : *plan.to_read) {
        for (const auto& [off, len] : extents) {
          ceph::bufferlist bytes;
          bytes.append(std::string(len, 'z'));
          base.insert_in_shard(shard, off, bytes);
        }
      }
    }
    auto data = ECWriteJournal::assemble_flush(stripe, sinfo, base);
    std::string expected(stripe.valid_bytes, 'z');
    for (const auto& [off, bytes] : stripe.blocks) {
      expected.replace(off, bytes.length(), bytes.to_str());
    }
    expected.resize(width(), '\0');
    std::string got;
    for (unsigned raw = 0; raw < GetParam(); ++raw) {
      ceph::bufferlist chunk;
      data.get_buffer(sinfo.get_shard(raw_shard_id_t(raw)),
                      stripe.offset / GetParam(), 4096, chunk);
      EXPECT_EQ(4096u, chunk.length());
      got += chunk.to_str();
    }
    EXPECT_EQ(expected, got);
  }
};

TEST_P(JournalFlushTest, OneKNeedsPartialPageAndOtherDataChunks)
{
  auto s = stripe(width());
  write(s, 1024, 1024);
  verify(std::move(s), width());
}

TEST_P(JournalFlushTest, FourKNeedsOnlyOtherDataChunks)
{
  auto s = stripe(width());
  write(s, 4096, 4096);
  verify(std::move(s), width() - 4096);
}

TEST_P(JournalFlushTest, FullyCoveredStripeNeedsNoReads)
{
  for (unsigned size : {1024u, 4096u}) {
    auto s = stripe(width() * 2);
    for (uint64_t off = 0; off < width(); off += size) {
      write(s, off, size);
    }
    verify(std::move(s), 0);
  }
}

TEST_P(JournalFlushTest, UnalignedFourKPreservesBothPartialPages)
{
  // Admission permits 1 KiB alignment, not just page-aligned 4 KiB writes.
  // Exercise both ends of a data chunk and nonzero logical stripe offsets.
  for (uint64_t off = 1024; off + 4096 <= width(); off += 4096) {
    auto s = stripe(width());
    write(s, off, 4096);
    verify(std::move(s), width());
  }
}

TEST_P(JournalFlushTest, MetadataOnlyPlanNeedsNoReadsOrDataWrites)
{
  auto s = stripe();
  pg_pool_t pool;
  pool.set_flag(pg_pool_t::FLAG_EC_OPTIMIZATIONS);
  ECUtil::stripe_info_t sinfo(GetParam(), 3, width(), &pool);
  shard_id_set shards;
  shards.insert_range(shard_id_t(0), GetParam() + 3);
  object_info_t oi;
  oi.size = s.object_size;
  PGTransaction::ObjectOperation update;
  // The foreground journal hook leaves the OI update but removes data writes.
  ceph::bufferlist attrs;
  encode(oi, attrs, 0);
  update.attr_updates[OI_ATTR] = attrs;
  ECTransaction::WritePlanObj plan(s.object, update, sinfo, shards, shards,
    false, oi.size, oi, std::nullopt, 1);
  EXPECT_EQ(0u, reads(plan));
  EXPECT_TRUE(plan.will_write.empty());
  EXPECT_EQ(oi.size, plan.orig_size);
  EXPECT_EQ(oi.size, plan.projected_size);
}

TEST_P(JournalFlushTest, TailReadsOnlyExistingPages)
{
  auto s = stripe(width(), 4609);
  write(s, 1024, 1024);
  verify(std::move(s), 8192);
}

TEST_P(JournalFlushTest, FullyCoveredTailNeedsNoReads)
{
  auto s = stripe(width(), 4096);
  write(s, 0, 4096);
  verify(std::move(s), 0);
}

TEST_P(JournalFlushTest, SparseFragmentsPreserveUntouchedBytes)
{
  auto s = stripe();
  write(s, 0, 1024);
  write(s, 2048, 1024);
  write(s, 8192, 4096);
  write(s, width() - 1024, 1024);
  verify(std::move(s), width() - 4096);
}

TEST_P(JournalFlushTest, MissingLiveBaseDataFailsLoudly)
{
  auto s = stripe();
  write(s, 0, 1024);
  ECUtil::stripe_info_t sinfo(GetParam(), 3, width());
  ECUtil::shard_extent_map_t missing(&sinfo);
  ASSERT_DEATH(ECWriteJournal::assemble_flush(s, sinfo, missing), "FAILED ceph_assert");
}

INSTANTIATE_TEST_SUITE_P(EC8plus3And12plus3, JournalFlushTest,
                        testing::Values(8, 12));
} // anonymous namespace