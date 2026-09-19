// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// SPDX-License-Identifier: LGPL-2.1-or-later
#include <algorithm>
#include <cerrno>
#include <limits>
#include <numeric>
#include <random>
#include <stdexcept>
#include <tuple>

#include <gtest/gtest.h>

#include "osd/ECWriteJournal.h"

using namespace ECWriteJournal;
using Transaction = ceph::os::Transaction;

namespace {
ceph::bufferlist bytes(size_t n, char c)
{
  ceph::bufferlist bl;
  bl.append(std::string(n, c));
  return bl;
}

hobject_t object(const std::string& name = "data")
{
  return hobject_t(object_t(name), "", CEPH_NOSNAP, 0, 1, "");
}

ghobject_t prefix()
{
  return ghobject_t(object("ec-journal-test-run"), ghobject_t::NO_GEN,
                   shard_id_t(0));
}

class JournalTest : public testing::TestWithParam<unsigned> {
 protected:
  Limits limits() const {
    Limits limits;
    limits.stripe_width = GetParam() * 4096;
    return limits;
  }
  Journal journal() const { return Journal(coll_t(), prefix(), limits()); }
  uint64_t width() const { return limits().stripe_width; }
  Ticket append(Journal& journal, uint64_t off, uint64_t len, char c = 'x',
                const hobject_t& oid = object(), bool durable = true) {
    Transaction t;
    Ticket ticket;
    EXPECT_EQ(0, journal.append(oid, eversion_t(1, next_version++),
                              16 * width(), off, bytes(len, c), t, &ticket));
    if (durable) {
      // Simulated ObjectStore commit. These unit tests do NOT test fsync.
      EXPECT_EQ(0, journal.committed(ticket, 0));
    }
    return ticket;
  }
  void drain(Journal& journal, const Flush& flush) {
    Transaction t;
    EXPECT_EQ(0, journal.finish_flush(flush.segment, 0, t));
    auto i = t.begin();
    ASSERT_TRUE(i.have_op());
    auto op = i.decode_op();
    EXPECT_EQ(Transaction::OP_REMOVE, op->op);
    EXPECT_EQ(journal.segment_object(flush.segment), i.get_oid(op->oid));
    EXPECT_FALSE(i.have_op());
    EXPECT_EQ(0, journal.trimmed(flush.segment, 0));
  }
 private:
  uint64_t next_version = 1;
};

TEST_P(JournalTest, EmptySealAndDrain)
{
  auto j = journal();
  j.seal();
  EXPECT_FALSE(j.begin_flush());
  EXPECT_TRUE(j.empty());
  EXPECT_EQ(0u, j.bytes());
}

TEST_P(JournalTest, ExactPayloadInAppendOnlyObjectStoreRecords)
{
  auto j = journal();
  Transaction t;
  Ticket first, second;
  ASSERT_EQ(0, j.append(object(), eversion_t(1, 1), width(), 1024,
                        bytes(1024, 'a'), t, &first));
  ASSERT_EQ(0, j.append(object(), eversion_t(1, 2), width(), 4096,
                        bytes(4096, 'b'), t, &second));
  EXPECT_EQ(first.segment, second.segment);
  auto it = t.begin();
  uint64_t end = 0;
  for (auto [ticket, off, len, value] : {
         std::tuple{first, 1024u, 1024u, 'a'},
         std::tuple{second, 4096u, 4096u, 'b'}}) {
    ASSERT_TRUE(it.have_op());
    auto op = it.decode_op();
    EXPECT_EQ(Transaction::OP_WRITE, op->op);
    EXPECT_EQ(j.segment_object(ticket.segment), it.get_oid(op->oid));
    EXPECT_EQ(end, op->off);
    end += op->len;
    ceph::bufferlist encoded;
    it.decode_bl(encoded);
    EXPECT_EQ(op->len, encoded.length());
    Record record;
    auto p = encoded.cbegin();
    decode(record, p);
    EXPECT_TRUE(p.end());
    EXPECT_EQ(ticket.sequence, record.sequence);
    EXPECT_EQ(object(), record.object);
    EXPECT_EQ(eversion_t(1, ticket.sequence), record.version);
    EXPECT_EQ(width(), record.object_size);
    EXPECT_EQ(off, record.offset);
    EXPECT_EQ(std::string(len, value), record.data.to_str());
  }
  EXPECT_FALSE(it.have_op());
  EXPECT_EQ(end, j.bytes());
  j.seal();
  EXPECT_FALSE(j.begin_flush()); // preparing transactions isn't durability
  ASSERT_EQ(0, j.committed(second, 0));
  EXPECT_FALSE(j.begin_flush());
  ASSERT_EQ(0, j.committed(first, 0));
  EXPECT_TRUE(j.begin_flush());
}

TEST_P(JournalTest, WakeOnlyWhenOldestSealedSegmentBecomesDurable)
{
  auto j = journal();
  auto first = append(j, 0, 1024, 'a', object(), false);
  auto second = append(j, 1024, 1024, 'b', object(), false);
  EXPECT_TRUE(j.has_open_segment());
  EXPECT_FALSE(j.flush_ready());
  j.seal();
  EXPECT_FALSE(j.has_open_segment());
  auto next = append(j, 2048, 1024, 'c', object(), false);
  EXPECT_TRUE(j.has_open_segment());
  EXPECT_EQ(0, j.committed(next, 0));
  EXPECT_FALSE(j.flush_ready());
  EXPECT_EQ(0, j.committed(second, 0));
  EXPECT_FALSE(j.flush_ready());
  EXPECT_EQ(0, j.committed(first, 0));
  EXPECT_TRUE(j.flush_ready());
  auto flush = j.begin_flush();
  ASSERT_TRUE(flush);
  EXPECT_FALSE(j.flush_ready());
  j.seal();
  EXPECT_FALSE(j.has_open_segment());
  EXPECT_FALSE(j.flush_ready()); // newer durable segment cannot overtake flush
  drain(j, *flush);
  EXPECT_TRUE(j.flush_ready());
}

TEST_P(JournalTest, RandomOrderDistinctWritesCompleteStripeWithoutBaseReads)
{
  for (unsigned size : {1024u, 4096u}) {
    auto j = journal();
    std::vector<unsigned> slots(width() / size);
    std::iota(slots.begin(), slots.end(), 0);
    std::mt19937 rng(83);
    std::shuffle(slots.begin(), slots.end(), rng);
    std::string expected(width(), '?');
    for (unsigned slot : slots) {
      const char value = 'a' + slot % 26;
      append(j, slot * size, size, value);
      expected.replace(slot * size, size, std::string(size, value));
    }
    j.seal();
    auto flush = j.begin_flush();
    ASSERT_TRUE(flush);
    ASSERT_EQ(1u, flush->stripes.size());
    auto& stripe = flush->stripes.front();
    EXPECT_TRUE(stripe.full());
    EXPECT_TRUE(stripe.holes().empty());
    EXPECT_EQ(width(), stripe.dirty_bytes());
    EXPECT_EQ(expected, stripe.assemble().to_str());
    EXPECT_EQ(width() / size, flush->records);
    EXPECT_EQ(width(), flush->payload_bytes);
    drain(j, *flush);
    EXPECT_TRUE(j.empty());
    EXPECT_EQ(0u, j.bytes());
    EXPECT_EQ(0u, j.records());
  }
}

TEST_P(JournalTest, FourOneKWritesFillPageNotStripe)
{
  auto j = journal();
  for (unsigned slot = 0; slot < 4; ++slot) {
    append(j, slot * 1024, 1024);
  }
  j.seal();
  auto flush = j.begin_flush();
  ASSERT_TRUE(flush);
  const auto& stripe = flush->stripes.at(0);
  EXPECT_FALSE(stripe.full());
  EXPECT_EQ(4096u, stripe.dirty_bytes());
  ASSERT_EQ(1u, stripe.holes().size());
  EXPECT_EQ(std::make_pair(uint64_t(4096), width() - 4096), stripe.holes()[0]);
  EXPECT_THROW(stripe.assemble(), std::invalid_argument);
}

TEST_P(JournalTest, DuplicateWritesDoNotCreateCoverage)
{
  auto j = journal();
  for (unsigned n = 0; n < width() / 1024; ++n) {
    append(j, 1024, 1024, 'a' + n % 26);
  }
  j.seal();
  auto flush = j.begin_flush();
  ASSERT_TRUE(flush);
  EXPECT_EQ(1024u, flush->stripes.at(0).dirty_bytes());
  EXPECT_FALSE(flush->stripes.at(0).full());
  EXPECT_EQ(width(), flush->payload_bytes);
}

TEST_P(JournalTest, OverlapsUseAppendNotCommitOrderAndPreserveHoles)
{
  auto j = journal();
  auto first = append(j, width(), 4096, 'a', object(), false);
  auto second = append(j, width() + 1024, 1024, 'b', object(), false);
  auto third = append(j, width() + 4096, 1024, 'c', object(), false);
  j.seal();
  EXPECT_EQ(0, j.committed(third, 0));
  EXPECT_EQ(0, j.committed(second, 0));
  EXPECT_FALSE(j.begin_flush());
  EXPECT_EQ(0, j.committed(first, 0));
  auto flush = j.begin_flush();
  ASSERT_TRUE(flush);
  const auto& stripe = flush->stripes.at(0);
  EXPECT_EQ(width(), stripe.offset);
  EXPECT_EQ(5120u, stripe.dirty_bytes());
  auto base = bytes(width(), 'z');
  std::string expected(width(), 'z');
  expected.replace(0, 4096, std::string(4096, 'a'));
  expected.replace(1024, 1024, std::string(1024, 'b'));
  expected.replace(4096, 1024, std::string(1024, 'c'));
  EXPECT_EQ(expected, stripe.assemble(&base).to_str());
  auto short_base = bytes(1024, 'z');
  EXPECT_THROW(stripe.assemble(&short_base), std::invalid_argument);
}

TEST_P(JournalTest, ObjectsAndStripesAreIndependent)
{
  auto j = journal();
  append(j, 0, 1024, 'a', object("one"));
  append(j, width(), 1024, 'b', object("one"));
  append(j, 0, 1024, 'c', object("two"));
  j.seal();
  auto flush = j.begin_flush();
  ASSERT_TRUE(flush);
  ASSERT_EQ(3u, flush->stripes.size());
  for (const auto& stripe : flush->stripes) {
    EXPECT_EQ(1024u, stripe.dirty_bytes());
    EXPECT_FALSE(stripe.full());
  }
}

TEST_P(JournalTest, NewerWritesSurviveOlderFlushAndTrim)
{
  auto j = journal();
  append(j, 0, 1024, 'a');
  j.seal();
  auto old = j.begin_flush();
  ASSERT_TRUE(old);
  append(j, 0, 1024, 'b');
  j.seal();
  EXPECT_FALSE(j.begin_flush());
  auto base = bytes(width(), 'z');
  auto old_data = old->stripes.at(0).assemble(&base);
  EXPECT_EQ(std::string(1024, 'a'), old_data.to_str().substr(0, 1024));
  drain(j, *old);
  EXPECT_EQ(1u, j.records());
  auto newer = j.begin_flush();
  ASSERT_TRUE(newer);
  EXPECT_NE(old->segment, newer->segment);
  EXPECT_EQ(std::string(1024, 'b'),
            newer->stripes.at(0).assemble(&old_data).to_str().substr(0, 1024));
  drain(j, *newer);
  EXPECT_TRUE(j.empty());
}

TEST_P(JournalTest, BackpressureBudgetReleasedOnlyAfterTrimCommit)
{
  auto l = limits();
  l.max_records = 1;
  Journal j(coll_t(), prefix(), l);
  append(j, 0, 1024);
  j.seal();
  auto flush = j.begin_flush();
  ASSERT_TRUE(flush);
  auto used = j.bytes();
  Transaction t;
  Ticket rejected;
  EXPECT_EQ(-EAGAIN, j.append(object(), eversion_t(1, 2), width(), 1024,
                              bytes(1024, 'b'), t, &rejected));
  EXPECT_TRUE(t.empty());
  EXPECT_EQ(used, j.bytes());
  ASSERT_EQ(0, j.finish_flush(flush->segment, 0, t));
  EXPECT_EQ(used, j.bytes());
  EXPECT_EQ(1u, j.records());
  EXPECT_FALSE(j.begin_flush());
  EXPECT_EQ(0, j.trimmed(flush->segment, 0));
  EXPECT_EQ(0u, j.bytes());
  EXPECT_EQ(0u, j.records());
  append(j, 1024, 1024);
}

TEST_P(JournalTest, ByteLimitIncludesRecordMetadata)
{
  auto l = limits();
  l.segment_bytes = l.max_bytes = 2048;
  Journal j(coll_t(), prefix(), l);
  append(j, 0, 1024);
  EXPECT_GT(j.bytes(), 1024u);
  Transaction t;
  Ticket ignored;
  EXPECT_EQ(-EAGAIN, j.append(object(), eversion_t(1, 2), width(), 1024,
                              bytes(1024, 'x'), t, &ignored));
  EXPECT_TRUE(t.empty());
}

TEST_P(JournalTest, SegmentRotationIsBoundedAndOldestDrainsFirst)
{
  auto l = limits();
  l.segment_bytes = 2048;
  l.max_segments = 2;
  Journal j(coll_t(), prefix(), l);
  auto first = append(j, 0, 1024, 'a', object(), false);
  auto second = append(j, 1024, 1024, 'b');
  EXPECT_NE(first.segment, second.segment);
  EXPECT_FALSE(j.begin_flush()); // second commit can't bypass first segment
  Transaction t;
  Ticket ignored;
  EXPECT_EQ(-EAGAIN, j.append(object(), eversion_t(1, 3), width(), 2048,
                              bytes(1024, 'c'), t, &ignored));
  EXPECT_TRUE(t.empty());
  EXPECT_EQ(0, j.committed(first, 0));
  auto flush = j.begin_flush();
  ASSERT_TRUE(flush);
  EXPECT_EQ(first.segment, flush->segment);
  drain(j, *flush);
  j.seal();
  auto last = j.begin_flush();
  ASSERT_TRUE(last);
  EXPECT_EQ(second.segment, last->segment);
  drain(j, *last);
}

TEST_P(JournalTest, FailedFlushOrTrimDoesNotLoseJournal)
{
  auto j = journal();
  append(j, 0, 1024);
  j.seal();
  auto flush = j.begin_flush();
  ASSERT_TRUE(flush);
  const auto used = j.bytes();
  Transaction t;
  EXPECT_EQ(-EIO, j.finish_flush(flush->segment, -EIO, t));
  EXPECT_TRUE(t.empty());
  EXPECT_EQ(used, j.bytes());
  auto retry = j.begin_flush();
  ASSERT_TRUE(retry);
  EXPECT_EQ(flush->segment, retry->segment);
  EXPECT_EQ(0, j.finish_flush(retry->segment, 0, t));
  EXPECT_EQ(-EIO, j.trimmed(retry->segment, -EIO));
  EXPECT_EQ(used, j.bytes());
  drain(j, *retry); // retries segment removal, not the already committed data
  EXPECT_TRUE(j.empty());
}

TEST_P(JournalTest, FailedAppendStopsAdmissionAndFlush)
{
  auto j = journal();
  auto ticket = append(j, 0, 1024, 'x', object(), false);
  EXPECT_EQ(-EIO, j.committed(ticket, -EIO));
  EXPECT_EQ(-EIO, j.error());
  j.seal();
  EXPECT_FALSE(j.begin_flush());
  Transaction t;
  Ticket ignored;
  EXPECT_EQ(-EIO, j.append(object(), eversion_t(1, 2), width(), 1024,
                           bytes(1024, 'y'), t, &ignored));
  EXPECT_TRUE(t.empty());
  EXPECT_EQ(1u, j.records());
}

TEST_P(JournalTest, InvalidTransitionsDoNotReclaimRecords)
{
  auto j = journal();
  auto ticket = append(j, 0, 1024);
  EXPECT_EQ(-EALREADY, j.committed(ticket, 0));
  EXPECT_EQ(-ENOENT, j.committed(Ticket{ticket.segment, 999}, 0));
  Transaction t;
  EXPECT_EQ(-EINVAL, j.finish_flush(ticket.segment, 0, t));
  EXPECT_EQ(-EINVAL, j.trimmed(ticket.segment, 0));
  EXPECT_EQ(-ENOENT, j.finish_flush(999, 0, t));
  EXPECT_TRUE(t.empty());
  EXPECT_EQ(1u, j.records());
}

TEST_P(JournalTest, InvalidWritesHaveNoSideEffects)
{
  auto j = journal();
  for (auto [off, len] : {
         std::pair{uint64_t(0), uint64_t(512)},
         std::pair{uint64_t(1), uint64_t(1024)},
         std::pair{width() - 1024, uint64_t(4096)}, // crosses stripe/EOF
         std::pair{width(), uint64_t(1024)},
         std::pair{std::numeric_limits<uint64_t>::max() - 1023, uint64_t(1024)}}) {
    Transaction t;
    Ticket ignored;
    EXPECT_EQ(-EINVAL, j.append(object(), eversion_t(1, 1), width(), off,
                                bytes(len, 'x'), t, &ignored));
    EXPECT_TRUE(t.empty());
    EXPECT_TRUE(j.empty());
    EXPECT_EQ(0u, j.bytes());
  }
}

TEST_P(JournalTest, RandomOverwritesMatchReferenceAfterDrain)
{
  // Mechanism test, not an IOPS benchmark: random duplicates, both write sizes,
  // nonzero holes, multiple objects/stripes and deliberately reversed callbacks.
  auto j = journal();
  std::mt19937 rng(123);
  std::map<hobject_t, std::string> expected;
  for (const auto& name : {"one", "two"}) {
    expected.emplace(object(name), std::string(width() * 3, 'z'));
  }
  std::vector<Ticket> tickets;
  for (unsigned n = 0; n < 300; ++n) {
    const uint64_t size = rng() % 2 ? 1024 : 4096;
    const uint64_t off = (rng() % (width() * 3 / size)) * size;
    auto oid = object(rng() % 2 ? "one" : "two");
    const char value = 'a' + rng() % 26;
    tickets.push_back(append(j, off, size, value, oid, false));
    expected.at(oid).replace(off, size, std::string(size, value));
  }
  j.seal();
  for (auto i = tickets.rbegin(); i != tickets.rend(); ++i) {
    EXPECT_EQ(0, j.committed(*i, 0));
  }
  auto flush = j.begin_flush();
  ASSERT_TRUE(flush);
  auto base = bytes(width(), 'z');
  for (const auto& stripe : flush->stripes) {
    EXPECT_EQ(expected.at(stripe.object).substr(stripe.offset, width()),
              stripe.assemble(&base).to_str());
  }
  drain(j, *flush);
  EXPECT_TRUE(j.empty());
}

TEST_P(JournalTest, ObjectTailPreservesDataAndPadsOnlyBeyondEOF)
{
  auto j = journal();
  // A 4 MiB RBD object is not a multiple of a 12+3/4K stripe. Also exercise
  // a non-1K-aligned EOF: no full-stripe read may be requested beyond it.
  const uint64_t size = width() + 4609;
  Transaction t;
  Ticket ticket;
  ASSERT_EQ(0, j.append(object(), eversion_t(1, 1), size, width() + 1024,
                        bytes(1024, 'x'), t, &ticket));
  ASSERT_EQ(0, j.committed(ticket, 0));
  j.seal();
  auto flush = j.begin_flush();
  ASSERT_TRUE(flush);
  const auto& stripe = flush->stripes.at(0);
  EXPECT_EQ(4609u, stripe.valid_bytes);
  EXPECT_FALSE(stripe.full());
  for (const auto& [off, len] : stripe.holes()) {
    EXPECT_LE(off + len, size);
  }
  auto base = bytes(4609, 'z');
  std::string expected(4609, 'z');
  expected.replace(1024, 1024, std::string(1024, 'x'));
  expected.resize(width(), '\0');
  EXPECT_EQ(expected, stripe.assemble(&base).to_str());
}

TEST_P(JournalTest, FullyCoveredTailNeedsNoBaseRead)
{
  auto j = journal();
  Transaction t;
  Ticket ticket;
  ASSERT_EQ(0, j.append(object(), eversion_t(1, 1), width() + 1024, width(),
                        bytes(1024, 'x'), t, &ticket));
  ASSERT_EQ(0, j.committed(ticket, 0));
  j.seal();
  auto flush = j.begin_flush();
  ASSERT_TRUE(flush);
  const auto& stripe = flush->stripes.at(0);
  EXPECT_TRUE(stripe.full());
  EXPECT_TRUE(stripe.holes().empty());
  std::string expected(1024, 'x');
  expected.resize(width(), '\0');
  EXPECT_EQ(expected, stripe.assemble().to_str());
}

INSTANTIATE_TEST_SUITE_P(EC4plus3And8plus3And12plus3, JournalTest,
                        testing::Values(4, 8, 12));

TEST(ECWriteJournal, RejectInvalidGeometryAndLimits)
{
  for (uint64_t width : {0u, 1024u, 4097u}) {
    Limits l;
    l.stripe_width = width;
    EXPECT_THROW(Journal(coll_t(), prefix(), l), std::invalid_argument);
  }
  Limits l;
  l.max_bytes = l.segment_bytes - 1;
  EXPECT_THROW(Journal(coll_t(), prefix(), l), std::invalid_argument);
}

TEST(ECWriteJournal, TruncatedRecordCannotDecode)
{
  Record record{1, object(), eversion_t(1, 1), 32768, 1024, bytes(1024, 'x')};
  ceph::bufferlist encoded;
  encode(record, encoded);
  ceph::bufferlist truncated;
  truncated.substr_of(encoded, 0, encoded.length() - 1);
  auto p = truncated.cbegin();
  Record decoded;
  EXPECT_THROW(decode(decoded, p), ceph::buffer::error);
}
} // anonymous namespace