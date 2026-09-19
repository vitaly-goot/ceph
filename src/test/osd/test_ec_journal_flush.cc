// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// SPDX-License-Identifier: LGPL-2.1-or-later
#include <gtest/gtest.h>
#include <random>
#include "osd/ECJournalFlush.h"
#include "osd/ECJournalQueue.h"
#include "osd/osd_internal_types.h"

namespace {
// Uses the same distinct variant alternatives as ConfigProxy. A regression to
// get_val<uint64_t> for either size setting throws here, just as in an OSD.
struct JournalConfig {
  std::map<std::string, Option::value_t> values = {
    {"osd_ec_journal_poc_segment_bytes", Option::size_t{8192}},
    {"osd_ec_journal_poc_max_bytes", Option::size_t{32768}},
    {"osd_ec_journal_poc_max_records", uint64_t{128}},
  };
  template<typename T> T get_val(const std::string& key) const {
    return std::get<T>(values.at(key));
  }
};

TEST(JournalAdmission, SizeOptionVariantAndInvalidLimits)
{
  JournalConfig conf;
  auto limits = ECWriteJournal::read_limits(conf, 32768);
  EXPECT_TRUE(limits.valid());
  EXPECT_EQ(8192u, limits.segment_bytes);
  EXPECT_EQ(32768u, limits.max_bytes);
  EXPECT_EQ(4u, limits.max_segments);
  conf.values["osd_ec_journal_poc_max_bytes"] = Option::size_t{4096};
  EXPECT_FALSE(ECWriteJournal::read_limits(conf, 32768).valid());
  conf.values["osd_ec_journal_poc_segment_bytes"] = Option::size_t{0};
  EXPECT_FALSE(ECWriteJournal::read_limits(conf, 32768).valid());
  EXPECT_FALSE(ECWriteJournal::read_limits(conf, 0).valid());
}

TEST(JournalAdmission, UnsupportedProfilesRefuseEnrollment)
{
  EXPECT_TRUE(ECWriteJournal::supported_geometry(4, 3, 4096));
  EXPECT_TRUE(ECWriteJournal::supported_geometry(8, 3, 4096));
  EXPECT_TRUE(ECWriteJournal::supported_geometry(12, 3, 4096));
  EXPECT_FALSE(ECWriteJournal::supported_geometry(6, 3, 4096));
  EXPECT_FALSE(ECWriteJournal::supported_geometry(4, 2, 4096));
  EXPECT_FALSE(ECWriteJournal::supported_geometry(4, 3, 8192));
  EXPECT_FALSE(ECWriteJournal::supported_geometry(8, 2, 4096));
  EXPECT_FALSE(ECWriteJournal::supported_geometry(12, 3, 8192));
}

TEST(JournalAdmission, ErrorLogBarrierCannotPassBlockedWrite)
{
  ECWriteJournal::AdmissionQueue<int> pending;
  std::vector<int> submitted;
  pending.push(1);
  pending.ordered([&] { submitted.push_back(2); });
  pending.push(3);
  auto forward = [](std::function<void()> cb) { cb(); };
  // Repeated pressure/deadline wakes leave version 1 blocked, not version 2
  // appended to the PG log ahead of it, even with an idle extent cache.
  for (unsigned i = 0; i < 4; ++i) {
    EXPECT_EQ(1, pending.next(forward));
    EXPECT_TRUE(submitted.empty());
  }
  submitted.push_back(1);
  pending.pop();
  EXPECT_EQ(3, pending.next(forward));
  EXPECT_EQ((std::vector<int>{1, 2}), submitted);
  submitted.push_back(3);
  pending.pop();
  EXPECT_FALSE(pending.next(forward));
}

TEST(JournalAdmission, OrderedCallbackMayEnqueueAndResetCancels)
{
  ECWriteJournal::AdmissionQueue<int> pending;
  pending.ordered([&] { pending.push(4); });
  EXPECT_EQ(4, pending.next([](std::function<void()> cb) { cb(); }));
  bool called = false;
  pending.ordered([&] { called = true; });
  pending.clear();
  EXPECT_FALSE(pending.next([](std::function<void()> cb) { cb(); }));
  EXPECT_FALSE(called);
}

TEST(JournalAdmission, ShutdownWaitsForTrimAndPendingAdmission)
{
  ECWriteJournal::DrainWaiters drain;
  unsigned completed = 0;
  EXPECT_FALSE(drain.requested());
  drain.request([&](int r) { EXPECT_EQ(0, r); ++completed; });
  EXPECT_TRUE(drain.requested());
  drain.maybe_finish(false, true); // data committed, but trim still outstanding
  drain.maybe_finish(true, false); // journal gone, but queued mutations remain
  EXPECT_EQ(0u, completed);
  drain.maybe_finish(true, true);
  drain.maybe_finish(true, true);
  EXPECT_EQ(1u, completed);
  EXPECT_TRUE(drain.requested()); // cannot enroll again before actual stop
}

TEST(JournalAdmission, ShutdownRetryAndCancellationCompleteOnce)
{
  ECWriteJournal::DrainWaiters drain;
  unsigned completed = 0;
  // First shutdown caller can time out; retry registers another owned waiter.
  for (unsigned i = 0; i < 2; ++i) {
    drain.request([&](int r) { EXPECT_EQ(-ECANCELED, r); ++completed; });
  }
  drain.finish(-ECANCELED);
  drain.maybe_finish(true, true);
  EXPECT_EQ(2u, completed);
  EXPECT_TRUE(drain.requested());
}

class JournalFlushTest : public testing::TestWithParam<unsigned> {
 protected:
  uint64_t width() const { return GetParam() * 4096; }
  PGTransaction::ObjectOperation head_write(uint64_t size, uint64_t len = 1024) {
    PGTransaction::ObjectOperation update;
    object_info_t oi;
    oi.size = size;
    ceph::bufferlist attr;
    encode(oi, attr, 0);
    update.attr_updates[OI_ATTR] = attr;
    attr.clear();
    encode(SnapSet{}, attr);
    update.attr_updates[SS_ATTR] = attr;
    ceph::bufferlist data;
    data.append(std::string(len, 'x'));
    update.buffer_updates.insert(0, len,
      PGTransaction::ObjectOperation::BufferUpdate::Write{data, 0});
    return update;
  }
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
  auto update = head_write(oi.size);
  ASSERT_TRUE(ECWriteJournal::eligible_overwrite(s.object, update, oi.size, width()));
  // The real foreground hook keeps BOTH finish_ctx attrs and removes the data.
  update.buffer_updates.clear();
  ECTransaction::WritePlanObj plan(s.object, update, sinfo, shards, shards,
    false, oi.size, oi, std::nullopt, 1);
  EXPECT_EQ(0u, reads(plan));
  EXPECT_TRUE(plan.will_write.empty());
  EXPECT_EQ(oi.size, plan.orig_size);
  EXPECT_EQ(oi.size, plan.projected_size);
}

TEST_P(JournalFlushTest, RealHeadAttrsEnrollButCompoundAndSnapshotWritesDoNot)
{
  const auto object = stripe().object;
  for (auto size : {1024u, 4096u}) {
    auto update = head_write(width(), size);
    EXPECT_TRUE(ECWriteJournal::eligible_overwrite(object, update, width(), width()));
    EXPECT_FALSE(ECWriteJournal::eligible_overwrite(object, update, width() - 1024, width()));
    update.attr_updates.erase(SS_ATTR);
    EXPECT_FALSE(ECWriteJournal::eligible_overwrite(object, update, width(), width()));
    update = head_write(width(), size);
    update.attr_updates["user-attr"] = ceph::bufferlist{};
    EXPECT_FALSE(ECWriteJournal::eligible_overwrite(object, update, width(), width()));
    update = head_write(width(), size);
    update.truncate = std::make_pair(uint64_t{0}, uint64_t{0});
    EXPECT_FALSE(ECWriteJournal::eligible_overwrite(object, update, width(), width()));
    update = head_write(width(), size);
    SnapSet ss;
    ss.seq = 1;
    ceph::bufferlist attr;
    encode(ss, attr);
    update.attr_updates[SS_ATTR] = attr;
    EXPECT_FALSE(ECWriteJournal::eligible_overwrite(object, update, width(), width()));
  }
}

TEST_P(JournalFlushTest, DeferredWriteAndTruncateKeepSubmittedSizes)
{
  const auto object = stripe().object;
  PGTransaction t;
  auto obc = std::make_shared<ObjectContext>();
  t.obc_map[object] = obc;
  obc->obs.oi.size = 2 * width();
  const auto write_oi = ECTransaction::snapshot_object_info(t);
  obc->obs.oi.size = 1024;
  const auto truncate_oi = ECTransaction::snapshot_object_info(t);
  // A newer client reextends the live OBC while both older ops are queued.
  obc->obs.oi.size = 4 * width();
  auto update = head_write(2 * width());
  update.buffer_updates.clear(); // journaled metadata-only foreground op
  pg_pool_t pool;
  pool.set_flag(pg_pool_t::FLAG_EC_OPTIMIZATIONS);
  ECUtil::stripe_info_t sinfo(GetParam(), 3, width(), &pool);
  shard_id_set shards;
  shards.insert_range(shard_id_t(0), GetParam() + 3);
  ECTransaction::WritePlanObj write_plan(object, update, sinfo, shards, shards,
    false, 2 * width(), write_oi.at(object), std::nullopt, 1);
  EXPECT_EQ(2 * width(), write_plan.projected_size);
  EXPECT_EQ(0u, reads(write_plan));
  EXPECT_TRUE(write_plan.will_write.empty());
  update.truncate = std::make_pair(uint64_t{1024}, uint64_t{1024});
  ECTransaction::WritePlanObj truncate_plan(object, update, sinfo, shards, shards,
    false, write_plan.projected_size, truncate_oi.at(object), std::nullopt, 1);
  EXPECT_EQ(2 * width(), truncate_plan.orig_size);
  EXPECT_EQ(1024u, truncate_plan.projected_size);
  EXPECT_TRUE(truncate_plan.invalidates_cache);
  EXPECT_GT(reads(truncate_plan), 0u);
}

TEST_P(JournalFlushTest, JournalGenerationsUseProductionShardAssembly)
{
  // Unlike Stripe::assemble (a reference model), this goes from real append
  // records through begin_flush, plan_flush and assemble_flush on every drain.
  auto object = stripe().object;
  ECWriteJournal::Limits limits;
  limits.stripe_width = width();
  ECWriteJournal::Journal journal(coll_t(), ghobject_t(object), limits);
  pg_pool_t pool;
  pool.set_flag(pg_pool_t::FLAG_EC_OPTIMIZATIONS);
  ECUtil::stripe_info_t sinfo(GetParam(), 3, width(), &pool);
  shard_id_set shards;
  shards.insert_range(shard_id_t(0), GetParam() + 3);
  std::string physical(width(), 'z');
  std::string expected = physical;
  std::mt19937 rng(71);
  for (unsigned generation = 0; generation < 3; ++generation) {
    for (unsigned n = 0; n < 65; ++n) {
      uint64_t off = (rng() % (width() / 1024)) * 1024;
      char value = 'a' + n % 26;
      ceph::bufferlist bytes;
      bytes.append(std::string(1024, value));
      ceph::os::Transaction txn;
      ECWriteJournal::Ticket ticket;
      ASSERT_EQ(0, journal.append(object, eversion_t(1, generation * 65 + n + 1),
        width(), off, bytes, txn, &ticket));
      ASSERT_EQ(0, journal.committed(ticket, 0));
      expected.replace(off, 1024, bytes.to_str());
    }
    journal.seal();
    auto flush = journal.begin_flush();
    ASSERT_TRUE(flush);
    ASSERT_EQ(1u, flush->stripes.size());
    auto& s = flush->stripes.front();
    auto plan = ECWriteJournal::plan_flush(s, sinfo, shards);
    ECUtil::shard_extent_map_t base(&sinfo);
    if (plan.to_read) {
      for (const auto& [shard, extents] : *plan.to_read) {
        for (const auto& [off, len] : extents) {
          ceph::bufferlist bytes;
          bytes.append(physical.substr(uint64_t(shard.id) * 4096 + off, len));
          base.insert_in_shard(shard, off, bytes);
        }
      }
    }
    auto data = ECWriteJournal::assemble_flush(s, sinfo, base);
    physical.clear();
    for (unsigned raw = 0; raw < GetParam(); ++raw) {
      ceph::bufferlist chunk;
      data.get_buffer(sinfo.get_shard(raw_shard_id_t(raw)), 0, 4096, chunk);
      physical += chunk.to_str();
    }
    EXPECT_EQ(expected, physical);
    ceph::os::Transaction trim;
    ASSERT_EQ(0, journal.finish_flush(flush->segment, 0, trim));
    ASSERT_EQ(0, journal.trimmed(flush->segment, 0));
  }
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

INSTANTIATE_TEST_SUITE_P(EC4plus3And8plus3And12plus3, JournalFlushTest,
                        testing::Values(4, 8, 12));
} // anonymous namespace