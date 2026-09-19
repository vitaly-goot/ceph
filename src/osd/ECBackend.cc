// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2013 Inktank Storage, Inc.
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include "ECBackend.h"

#include <iostream>

#include "ECInject.h"
#include "messages/MOSDPGPush.h"
#include "messages/MOSDPGPushReply.h"
#include "messages/MOSDECSubOpWrite.h"
#include "messages/MOSDECSubOpWriteReply.h"
#include "messages/MOSDECSubOpRead.h"
#include "messages/MOSDECSubOpReadReply.h"
#include "common/debug.h"
#include "ECMsgTypes.h"
#include "ECTypes.h"
#include "ECSwitch.h"
#include "ECJournalFlush.h"

#include "PrimaryLogPG.h"

#define dout_context cct
#define dout_subsys ceph_subsys_osd
#define DOUT_PREFIX_ARGS this
#undef dout_prefix
#define dout_prefix _prefix(_dout, this)

using std::dec;
using std::hex;
using std::less;
using std::list;
using std::make_pair;
using std::map;
using std::pair;
using std::ostream;
using std::set;
using std::string;
using std::unique_ptr;
using std::vector;

using ceph::bufferhash;
using ceph::bufferlist;
using ceph::bufferptr;
using ceph::ErasureCodeInterfaceRef;
using ceph::Formatter;

static ostream &_prefix(std::ostream *_dout, ECBackend *pgb) {
  return pgb->get_parent()->gen_dbg_prefix(*_dout);
}

static ostream& _prefix(std::ostream *_dout, ECBackend::ECRecoveryBackend *pgb) {
  return pgb->get_parent()->gen_dbg_prefix(*_dout);
}

struct ECBackend::ECRecoveryBackend::ECRecoveryHandle : public PGBackend::RecoveryHandle {
  list<ECCommon::RecoveryBackend::RecoveryOp> ops;
};

struct ECBackend::JournalState {
  ECBackend* backend;
  ECWriteJournal::Journal journal;
  std::deque<std::shared_ptr<ECClassicalOp>> pending;
  std::optional<ECWriteJournal::Flush> flush;
  size_t next_stripe = 0;
  bool flush_in_progress = false;
  bool trimming = false;
  bool pumping = false;
  uint64_t open_segment = 0;
  std::map<std::pair<hobject_t, uint64_t>, interval_set<uint64_t>> coverage;
  ceph::mono_clock::time_point open_since = ceph::mono_clock::now();
  ceph::mono_clock::time_point wake_at{};
  uint64_t flush_ms;
  uint64_t admitted = 0;
  uint64_t acked = 0;
  uint64_t full_stripes = 0;
  uint64_t partial_stripes = 0;
  uint64_t planned_read_bytes = 0;
  uint64_t flushed_bytes = 0;
  uint64_t pressure_events = 0;

  struct Timer final : common::intrusive_timer::callback_t {
    JournalState* state;
    explicit Timer(JournalState* state) : state(state) {}
    void lock() override { state->backend->parent->pg_lock(); }
    void unlock() override { state->backend->parent->pg_unlock(); }
    void add_ref() override { state->backend->parent->pg_add_ref(); }
    void dec_ref() override { state->backend->parent->pg_dec_ref(); }
    void invoke() override { state->backend->pump_journal(); }
  } timer{this};

  JournalState(ECBackend* backend, ghobject_t prefix,
               ECWriteJournal::Limits limits, uint64_t flush_ms)
    : backend(backend), journal(backend->switcher->coll, std::move(prefix), limits),
      flush_ms(flush_ms) {}
};

ECBackend::ECBackend(
  PGBackend::Listener *pg,
  CephContext *cct,
  ErasureCodeInterfaceRef ec_impl,
  uint64_t stripe_width,
  ECSwitch *s,
  ECExtentCache::LRU &ec_extent_cache_lru)
  : parent(pg), cct(cct), switcher(s),
#ifdef WITH_CRIMSON
    read_pipeline(cct, ec_impl, this->sinfo, get_parent()->get_eclistener(), *this),
#else
    read_pipeline(cct, ec_impl, this->sinfo, get_parent()->get_eclistener()),
#endif
    rmw_pipeline(cct, ec_impl, this->sinfo, get_parent()->get_eclistener(),
                 *this, ec_extent_cache_lru),
    recovery_backend(cct, switcher->coll, ec_impl, this->sinfo, read_pipeline,
                      get_parent(), this),
    ec_impl(ec_impl),
    sinfo(ec_impl, &(get_parent()->get_pool()), stripe_width) {

  /* EC makes some assumptions about how the plugin organises the *data* shards:
   * - The chunk size is constant for a particular profile.
   * - A stripe consists of k chunks.
   */
  ceph_assert((ec_impl->get_data_chunk_count() *
    ec_impl->get_chunk_size(stripe_width)) == stripe_width);
}

PGBackend::RecoveryHandle *ECBackend::open_recovery_op() {
  return static_cast<PGBackend::RecoveryHandle*>(
    recovery_backend.open_recovery_op());
}

ECBackend::ECRecoveryBackend::ECRecoveryHandle *ECBackend::ECRecoveryBackend::open_recovery_op() {
  return new ECRecoveryHandle;
}

void ECBackend::handle_recovery_push(
  const PushOp &op,
  RecoveryMessages *m,
  bool is_repair) {
  if (get_parent()->pg_is_remote_backfilling()) {
    get_parent()->pg_add_local_num_bytes(op.data.length());
    get_parent()->pg_add_num_bytes(op.data.length() * sinfo.get_k());
    dout(10) << __func__ << " " << op.soid
             << " add new actual data by " << op.data.length()
             << " add new num_bytes by " << op.data.length() * sinfo.get_k()
             << dendl;
  }

  recovery_backend.handle_recovery_push(op, m, is_repair);

  if (op.after_progress.data_complete &&
    !(get_parent()->pgb_is_primary()) &&
    get_parent()->pg_is_remote_backfilling()) {
    struct stat st;
    int r = switcher->store->stat(switcher->ch, ghobject_t(
                                    op.soid, ghobject_t::NO_GEN,
                                    get_parent()->whoami_shard().shard), &st);
    if (r == 0) {
      get_parent()->pg_sub_local_num_bytes(st.st_size);
      // XXX: This can be way overestimated for small objects
      get_parent()->pg_sub_num_bytes(st.st_size * sinfo.get_k());
      dout(10) << __func__ << " " << op.soid
               << " sub actual data by " << st.st_size
               << " sub num_bytes by " << st.st_size * sinfo.get_k()
               << dendl;
    }
  }
}

void ECBackend::ECRecoveryBackend::maybe_load_obc(
  const std::map<std::string, ceph::bufferlist, std::less<>>& raw_attrs,
  RecoveryOp &op)
{
  if (!op.obc) {
    // attrs only reference the origin bufferlist (decode from
    // ECSubReadReply message) whose size is much greater than attrs
    // in recovery. If obc cache it (get_obc maybe cache the attr),
    // this causes the whole origin bufferlist would not be free
    // until obc is evicted from obc cache. So rebuild the
    // bufferlist before cache it.
    for (map<string, bufferlist>::iterator it = op.xattrs.begin();
         it != op.xattrs.end();
         ++it) {
      it->second.rebuild();
    }
    // Need to remove ECUtil::get_hinfo_key() since it should not leak out
    // of the backend (see bug #12983)
    map<string, bufferlist, less<>> sanitized_attrs(op.xattrs);
    sanitized_attrs.erase(ECUtil::get_hinfo_key());
    op.obc = get_parent()->get_obc(op.hoid, sanitized_attrs);
    ceph_assert(op.obc);
    op.recovery_info.size = op.obc->obs.oi.size;
    op.recovery_info.oi = op.obc->obs.oi;
  }
}

struct SendPushReplies : public Context {
  PGBackend::Listener *l;
  epoch_t epoch;
  std::map<int, MOSDPGPushReply*> replies;

  SendPushReplies(
    PGBackend::Listener *l,
    epoch_t epoch,
    std::map<int, MOSDPGPushReply*> &in) : l(l), epoch(epoch) {
    replies.swap(in);
  }

  void finish(int) override {
    std::vector<std::pair<int, Message*>> messages;
    messages.reserve(replies.size());
    for (auto & reply : replies) {
      messages.push_back(reply);
    }
    if (!messages.empty()) {
      l->send_message_osd_cluster(messages, epoch);
    }
    replies.clear();
  }

  ~SendPushReplies() override {
    for (auto & [_, reply] : replies) {
      reply->put();
    }
    replies.clear();
  }
};

void ECBackend::ECRecoveryBackend::commit_txn_send_replies(
  ceph::os::Transaction &&txn,
  std::map<int, MOSDPGPushReply*> replies) {
  txn.register_on_complete(
    get_parent()->bless_context(
      new SendPushReplies(
        get_parent(),
        get_osdmap_epoch(),
        replies)));
  get_parent()->queue_transaction(std::move(txn));
}

void ECBackend::run_recovery_op(
  PGBackend::RecoveryHandle *_h,
  int priority) {
  ceph_assert(_h);
  auto &h = static_cast<ECBackend::ECRecoveryBackend::ECRecoveryHandle&>(*_h);
  recovery_backend.run_recovery_op(h, priority);
  switcher->send_recovery_deletes(priority, h.deletes);
  delete _h;
}

void ECBackend::ECRecoveryBackend::run_recovery_op(
  ECBackend::ECRecoveryBackend::ECRecoveryHandle &h,
  int priority) {
  RecoveryMessages m;
  for (list<RecoveryOp>::iterator i = h.ops.begin();
       i != h.ops.end();
       ++i) {
    dout(10) << __func__ << ": starting " << *i << dendl;
    ceph_assert(!recovery_ops.count(i->hoid));
    RecoveryOp &op = recovery_ops.insert(make_pair(i->hoid, *i)).first->second;
    continue_recovery_op(op, &m);
  }
  dispatch_recovery_messages(m, priority);
}

int ECBackend::recover_object(
  const hobject_t &hoid,
  eversion_t v,
  ObjectContextRef head,
  ObjectContextRef obc,
  PGBackend::RecoveryHandle *_h) {
  auto *h = static_cast<ECBackend::ECRecoveryBackend::ECRecoveryHandle*>(_h);
  h->ops.push_back(recovery_backend.recover_object(hoid, v, head, obc));
  return 0;
}

bool ECBackend::can_handle_while_inactive(
  OpRequestRef _op) {
  return false;
}

bool ECBackend::_handle_message(
  OpRequestRef _op) {
  dout(10) << __func__ << ": " << *_op->get_req() << dendl;
  int priority = _op->get_req()->get_priority();
  switch (_op->get_req()->get_type()) {
  case MSG_OSD_EC_WRITE: {
    // NOTE: this is non-const because handle_sub_write modifies the embedded
    // ObjectStore::Transaction in place (and then std::move's it).  It does
    // not conflict with ECSubWrite's operator<<.
    MOSDECSubOpWrite *op = static_cast<MOSDECSubOpWrite*>(
      _op->get_nonconst_req());
    parent->maybe_preempt_replica_scrub(op->op.soid);
    handle_sub_write(op->op.from, _op, op->op, _op->pg_trace,
                     *get_parent()->get_eclistener());
    return true;
  }
  case MSG_OSD_EC_WRITE_REPLY: {
    const MOSDECSubOpWriteReply *op = static_cast<const MOSDECSubOpWriteReply*>(
      _op->get_req());
    handle_sub_write_reply(op->op.from, op->op, _op->pg_trace);
    return true;
  }
  case MSG_OSD_EC_READ: {
    auto op = _op->get_req<MOSDECSubOpRead>();
    MOSDECSubOpReadReply *reply = new MOSDECSubOpReadReply;
    reply->pgid = get_parent()->primary_spg_t();
    reply->map_epoch = switcher->get_osdmap_epoch();
    reply->min_epoch = get_parent()->get_interval_start_epoch();
    handle_sub_read(op->op.from, op->op, &(reply->op), _op->pg_trace);
    reply->trace = _op->pg_trace;
    get_parent()->send_message_osd_cluster(
      reply, _op->get_req()->get_connection());
    return true;
  }
  case MSG_OSD_EC_READ_REPLY: {
    // NOTE: this is non-const because handle_sub_read_reply steals resulting
    // buffers.  It does not conflict with ECSubReadReply operator<<.
    MOSDECSubOpReadReply *op = static_cast<MOSDECSubOpReadReply*>(
      _op->get_nonconst_req());
    handle_sub_read_reply(op->op.from, op->op, _op->pg_trace);
    // dispatch_recovery_messages() in the case of recovery_reads
    // is called via the `on_complete` callback
    return true;
  }
  case MSG_OSD_PG_PUSH: {
    auto op = _op->get_req<MOSDPGPush>();
    RecoveryMessages rm;
    for (vector<PushOp>::const_iterator i = op->pushes.begin();
         i != op->pushes.end();
         ++i) {
      handle_recovery_push(*i, &rm, op->is_repair);
    }
    recovery_backend.dispatch_recovery_messages(rm, priority);
    return true;
  }
  case MSG_OSD_PG_PUSH_REPLY: {
    const MOSDPGPushReply *op = static_cast<const MOSDPGPushReply*>(
      _op->get_req());
    RecoveryMessages rm;
    for (vector<PushReplyOp>::const_iterator i = op->replies.begin();
         i != op->replies.end();
         ++i) {
      recovery_backend.handle_recovery_push_reply(*i, op->from, &rm);
    }
    recovery_backend.dispatch_recovery_messages(rm, priority);
    return true;
  }
  default:
    return false;
  }
  return false;
}

struct SubWriteCommitted : public Context {
  ECBackend *pg;
  OpRequestRef msg;
  ceph_tid_t tid;
  eversion_t version;
  eversion_t last_complete;
  const ZTracer::Trace trace;

  SubWriteCommitted(
    ECBackend *pg,
    OpRequestRef msg,
    ceph_tid_t tid,
    eversion_t version,
    eversion_t last_complete,
    const ZTracer::Trace &trace)
    : pg(pg), msg(msg), tid(tid),
      version(version), last_complete(last_complete), trace(trace) {}

  void finish(int) override {
    if (msg)
      msg->mark_event("sub_op_committed");
    pg->sub_write_committed(tid, version, last_complete, trace);
  }
};

void ECBackend::sub_write_committed(
  ceph_tid_t tid, eversion_t version, eversion_t last_complete,
  const ZTracer::Trace &trace) {
  if (get_parent()->pgb_is_primary()) {
    ECSubWriteReply reply;
    reply.tid = tid;
    reply.last_complete = last_complete;
    reply.committed = true;
    reply.applied = true;
    reply.from = get_parent()->whoami_shard();
    handle_sub_write_reply(
      get_parent()->whoami_shard(),
      reply, trace);
  } else {
    get_parent()->update_last_complete_ondisk(last_complete);
    MOSDECSubOpWriteReply *r = new MOSDECSubOpWriteReply;
    r->pgid = get_parent()->primary_spg_t();
    r->map_epoch = switcher->get_osdmap_epoch();
    r->min_epoch = get_parent()->get_interval_start_epoch();
    r->op.tid = tid;
    r->op.last_complete = last_complete;
    r->op.committed = true;
    r->op.applied = true;
    r->op.from = get_parent()->whoami_shard();
    r->set_priority(CEPH_MSG_PRIO_HIGH);
    r->trace = trace;
    r->trace.event("sending sub op commit");
    get_parent()->send_message_osd_cluster(
      get_parent()->primary_shard().osd, r, switcher->get_osdmap_epoch());
  }
}

void ECBackend::handle_sub_write(
  pg_shard_t from,
  OpRequestRef msg,
  ECSubWrite &op,
  const ZTracer::Trace &trace,
  ECListener &) {
  if (msg) {
    msg->mark_event("sub_op_started");
  }
  trace.event("handle_sub_write");

  if (cct->_conf->bluestore_debug_inject_read_err &&
    ECInject::test_write_error3(op.soid)) {
    ceph_abort_msg("Error inject - OSD down");
  }
  if (!get_parent()->pgb_is_primary())
    get_parent()->update_stats(op.stats);
  ObjectStore::Transaction localt;
  if (!op.temp_added.empty()) {
    switcher->add_temp_objs(op.temp_added);
  }
  if (op.backfill_or_async_recovery) {
    for (set<hobject_t>::iterator i = op.temp_removed.begin();
         i != op.temp_removed.end();
         ++i) {
      dout(10) << __func__ << ": removing object " << *i
	       << " since we won't get the transaction" << dendl;
      localt.remove(
        switcher->coll,
        ghobject_t(
          *i,
          ghobject_t::NO_GEN,
          get_parent()->whoami_shard().shard));
    }
  }
  switcher->clear_temp_objs(op.temp_removed);
  dout(30) << __func__ << " missing before " <<
    get_parent()->get_log().get_missing().get_items() << dendl;
  // flag set to true during async recovery
  bool async = false;
  pg_missing_tracker_t pmissing = get_parent()->get_local_missing();
  if (pmissing.is_missing(op.soid)) {
    async = true;
    dout(30) << __func__ << " is_missing " <<
      pmissing.is_missing(op.soid) << dendl;
    for (auto &&e: op.log_entries) {
      dout(30) << " add_next_event entry " << e << dendl;
      get_parent()->add_local_next_event(e);
      dout(30) << " entry is_delete " << e.is_delete() << dendl;
    }
  }
  get_parent()->log_operation(
    std::move(op.log_entries),
    op.updated_hit_set_history,
    op.trim_to,
    op.pg_committed_to,
    op.pg_committed_to,
    !op.backfill_or_async_recovery,
    localt,
    async);

  if (!get_parent()->pg_is_undersized() &&
    get_parent()->whoami_shard().shard >= sinfo.get_k())
    op.t.set_fadvise_flag(CEPH_OSD_OP_FLAG_FADVISE_DONTNEED);

  localt.register_on_commit(
    get_parent()->bless_context(
      new SubWriteCommitted(
        this, msg, op.tid,
        op.at_version,
        get_parent()->get_info().last_complete, trace)));
  vector<ObjectStore::Transaction> tls;
  tls.reserve(2);
  tls.push_back(std::move(op.t));
  tls.push_back(std::move(localt));
  dout(20) << __func__ << " queue_transactions=";
  Formatter *f = Formatter::create("json");
  f->open_array_section("tls");
  for (ObjectStore::Transaction t: tls) {
    f->open_object_section("t");
    t.dump(f);
    f->close_section();
  }
  f->close_section();
  f->flush(*_dout);
  delete f;
  *_dout << dendl;
  get_parent()->queue_transactions(tls, msg);
  dout(30) << __func__ << " missing after" << get_parent()->get_log().
                                                            get_missing().
                                                            get_items() << dendl
  ;
  if (op.at_version != eversion_t()) {
    // dummy rollforward transaction doesn't get at_version (and doesn't advance it)
    get_parent()->op_applied(op.at_version);
  }
}

void ECBackend::handle_sub_read(
  pg_shard_t from,
  const ECSubRead &op,
  ECSubReadReply *reply,
  const ZTracer::Trace &trace) {
  trace.event("handle sub read");
  shard_id_t shard = get_parent()->whoami_shard().shard;
  for (auto &&[hoid, to_read]: op.to_read) {
    int r = 0;
    for (auto &&[offset, len, flags]: to_read) {
      bufferlist bl;
      auto &subchunks = op.subchunks.at(hoid);
      if ((subchunks.size() == 1) &&
        (subchunks.front().second == ec_impl->get_sub_chunk_count())) {
        dout(20) << __func__ << " case1: reading the complete chunk/shard." << dendl;
        r = switcher->store->read(
          switcher->ch,
          ghobject_t(hoid, ghobject_t::NO_GEN, shard),
          offset, len, bl, flags); // Allow EIO return
      } else {
        int subchunk_size =
          sinfo.get_chunk_size() / ec_impl->get_sub_chunk_count();
        dout(20) << __func__ << " case2: going to do fragmented read;"
		 << " subchunk_size=" << subchunk_size
		 << " chunk_size=" << sinfo.get_chunk_size() << dendl;
        bool error = false;
        for (int m = 0; m < (int)len && !error;
             m += sinfo.get_chunk_size()) {
          for (auto &&k: subchunks) {
            bufferlist bl0;
            r = switcher->store->read(
              switcher->ch,
              ghobject_t(hoid, ghobject_t::NO_GEN, shard),
              offset + m + (k.first) * subchunk_size,
              (k.second) * subchunk_size,
              bl0, flags);
            if (r < 0) {
              error = true;
              break;
            }
            bl.claim_append(bl0);
          }
        }
      }

      if (r < 0) {
        // if we are doing fast reads, it's possible for one of the shard
        // reads to cross paths with another update and get a (harmless)
        // ENOENT.  Suppress the message to the cluster log in that case.
        if (r == -ENOENT && get_parent()->get_pool().fast_read) {
          dout(5) << __func__ << ": Error " << r
		  << " reading " << hoid << ", fast read, probably ok"
		  << dendl;
        } else {
          get_parent()->clog_error() << "Error " << r
            << " reading object " << hoid;
          dout(5) << __func__ << ": Error " << r
		  << " reading " << hoid << dendl;
        }
        goto error;
      } else {
        dout(20) << __func__ << " read request=" << len << " r=" << r << " len="
          << bl.length() << dendl;
        reply->buffers_read[hoid].push_back(make_pair(offset, bl));
      }
    }
    continue;
  error:
    // Do NOT check osd_read_eio_on_bad_digest here.  We need to report
    // the state of our chunk in case other chunks could substitute.
    reply->buffers_read.erase(hoid);
    reply->errors[hoid] = r;
  }
  for (set<hobject_t>::iterator i = op.attrs_to_read.begin();
       i != op.attrs_to_read.end();
       ++i) {
    dout(10) << __func__ << ": fulfilling attr request on "
	     << *i << dendl;
    if (reply->errors.contains(*i))
      continue;
    int r = switcher->store->getattrs(
      switcher->ch,
      ghobject_t(
        *i, ghobject_t::NO_GEN, shard),
      reply->attrs_read[*i]);
    if (r < 0) {
      // If we read error, we should not return the attrs too.
      reply->attrs_read.erase(*i);
      reply->buffers_read.erase(*i);
      reply->errors[*i] = r;
    }
  }
  reply->from = get_parent()->whoami_shard();
  reply->tid = op.tid;
}

void ECBackend::handle_sub_read_n_reply(
  pg_shard_t from,
  ECSubRead &op,
  const ZTracer::Trace &trace)
{
  ECSubReadReply reply;
  handle_sub_read(from, op, &reply, trace);
  handle_sub_read_reply(from, reply, trace);
}

void ECBackend::handle_sub_write_reply(
  pg_shard_t from,
  const ECSubWriteReply &ec_write_reply_op,
  const ZTracer::Trace &trace) {
  RMWPipeline::OpRef &op = rmw_pipeline.tid_to_op_map.at(ec_write_reply_op.tid);
  if (ec_write_reply_op.committed) {
    trace.event("sub write committed");
    ceph_assert(op->pending_commits > 0);
    op->pending_commits--;
    if (from != get_parent()->whoami_shard()) {
      get_parent()->update_peer_last_complete_ondisk(
        from, ec_write_reply_op.last_complete);
    }
  }

  if (cct->_conf->bluestore_debug_inject_read_err &&
    (op->pending_commits == 1) &&
    ECInject::test_write_error2(op->hoid)) {
    std::string cmd =
      "{ \"prefix\": \"osd down\", \"ids\": [\"" + std::to_string(
        get_parent()->whoami()) + "\"] }";
    vector<std::string> vcmd{cmd};
    dout(0) << __func__ << " Error inject - marking OSD down" << dendl;
    get_parent()->start_mon_command(vcmd, {}, nullptr, nullptr, nullptr);
  }

  if (op->pending_commits == 0) {
    rmw_pipeline.try_finish_rmw();
  }
}

void ECBackend::handle_sub_read_reply(
    pg_shard_t from,
    ECSubReadReply &op,
    const ZTracer::Trace &trace) {
  trace.event("ec sub read reply");
  dout(10) << __func__ << ": reply " << op << dendl;
  map<ceph_tid_t, ReadOp>::iterator iter = read_pipeline.tid_to_read_map.
                                                         find(op.tid);
  if (iter == read_pipeline.tid_to_read_map.end()) {
    //canceled
    dout(20) << __func__ << ": dropped " << op << dendl;
    return;
  }
  ReadOp &rop = iter->second;
  if (cct->_conf->bluestore_debug_inject_read_err) {
    for (auto i = op.buffers_read.begin();
         i != op.buffers_read.end();
         ++i) {
      if (ECInject::test_read_error0(
        ghobject_t(i->first, ghobject_t::NO_GEN, op.from.shard))) {
        dout(0) << __func__ << " Error inject - EIO error for shard "
                << op.from.shard << dendl;
        op.buffers_read.erase(i->first);
        op.attrs_read.erase(i->first);
        op.errors[i->first] = -EIO;
        rop.debug_log.emplace_back(ECUtil::INJECT_EIO, op.from);
      }
    }
  }
  for (auto &&[hoid, offset_buffer_map]: op.buffers_read) {
    ceph_assert(!op.errors.contains(hoid));
    // If attribute error we better not have sent a buffer
    if (!rop.to_read.contains(hoid)) {
      rop.debug_log.emplace_back(ECUtil::CANCELLED, op.from);

      // We canceled this read! @see filter_read_op
      dout(20) << __func__ << " to_read skipping" << dendl;
      continue;
    }

    if (!rop.complete.contains(hoid)) {
      rop.complete.emplace(hoid, &sinfo);
    }

    auto &buffers_read = rop.complete.at(hoid).buffers_read;
    for (auto &&[offset, buffer_list]: offset_buffer_map) {
      buffers_read.insert_in_shard(from.shard, offset, buffer_list);
    }
    rop.debug_log.emplace_back(ECUtil::READ_DONE, op.from, buffers_read);

    // zero length reads may need to be zero padded during recovery
    if (!buffers_read.contains_shard(from.shard)) {
      rop.complete.at(hoid).zero_length_reads.insert(from.shard);
    }
  }
  for (auto &&[hoid, req]: rop.to_read) {
    if (!rop.complete.contains(hoid)) {
      rop.complete.emplace(hoid, &sinfo);
    }
    auto &complete = rop.complete.at(hoid);
    if (!req.shard_reads.contains(from.shard)) {
      continue;
    }
    const shard_read_t &read = req.shard_reads.at(from.shard);
    if (!complete.errors.contains(from)) {
      dout(20) << __func__ <<" read:" << read << dendl;
      complete.processed_read_requests[from.shard].union_of(read.extents);
    }
  }
  for (auto &&[hoid, attr]: op.attrs_read) {
    ceph_assert(!op.errors.count(hoid));
    // if read error better not have sent an attribute
    if (!rop.to_read.contains(hoid)) {
      // We canceled this read! @see filter_read_op
      dout(20) << __func__ << " to_read skipping" << dendl;
      continue;
    }
    if (!rop.complete.contains(hoid)) {
      rop.complete.emplace(hoid, &sinfo);
    }
    rop.complete.at(hoid).attrs.emplace();
    (*(rop.complete.at(hoid).attrs)).swap(attr);
  }
  for (auto &&[hoid, err]: op.errors) {
    if (!rop.complete.contains(hoid)) {
      rop.complete.emplace(hoid, &sinfo);
    }
    auto &complete = rop.complete.at(hoid);
    complete.errors.emplace(from, err);
    rop.debug_log.emplace_back(ECUtil::ERROR, op.from, complete.buffers_read);
    complete.buffers_read.erase_shard(from.shard);
    complete.processed_read_requests.erase(from.shard);
    // If there was an error for non-zero data on this shard, then we must also
    // ignore all zeros, or minimum_to_decode may conclude that it has enough
    // shards available.
    rop.to_read.at(hoid).zeros_for_decode.erase(from.shard);
    dout(20) << __func__ << " shard=" << from << " error=" << err << dendl;
  }

  map<pg_shard_t, set<ceph_tid_t>>::iterator siter =
    read_pipeline.shard_to_read_map.find(from);
  ceph_assert(siter != read_pipeline.shard_to_read_map.end());
  ceph_assert(siter->second.count(op.tid));
  siter->second.erase(op.tid);

  ceph_assert(rop.in_progress.count(from));
  rop.in_progress.erase(from);
  unsigned is_complete = 0;
  bool need_resend = false;
  bool all_sub_reads_done = rop.in_progress.empty();
  // For redundant reads check for completion as each shard comes in,
  // or in a non-recovery read check for completion once all the shards read.
  if (rop.do_redundant_reads || all_sub_reads_done) {
    for (auto &&[oid, read_result]: rop.complete) {
      shard_id_set have;
      read_result.processed_read_requests.populate_shard_id_set(have);
      shard_id_set dummy_minimum;
      shard_id_set want_to_read;
      rop.to_read.at(oid).shard_want_to_read.
          populate_shard_id_set(want_to_read);

      dout(20) << __func__ << " read_result: " << read_result << dendl;
      // If all reads are done, we can safely assume that zero buffers can
      // be applied.
      if (all_sub_reads_done) {
        rop.to_read.at(oid).zeros_for_decode.populate_shard_id_set(have);
      }

      int err = -EIO; // If attributes needed but not read.
      if (!rop.to_read.at(oid).want_attrs || rop.complete.at(oid).attrs) {
        err = ec_impl->minimum_to_decode(want_to_read, have, dummy_minimum,
                                                    nullptr);
      }

      if (err) {
        dout(20) << __func__ << " minimum_to_decode failed" << dendl;
        if (all_sub_reads_done) {
          // If we don't have enough copies, try other pg_shard_ts if available.
          // During recovery there may be multiple osds with copies of the same shard,
          // so getting EIO from one may result in multiple passes through this code path.

          rop.debug_log.emplace_back(ECUtil::REQUEST_MISSING, op.from);
          int r = read_pipeline.send_all_remaining_reads(oid, rop);
          if (r == 0 && !rop.do_redundant_reads) {
            // We found that new reads are required to do a decode.
            need_resend = true;
            continue;
          }
          // else insufficient shards are available, keep the errors.

          // Couldn't read any additional shards so handle as completed with errors
          // We don't want to confuse clients / RBD with objectstore error
          // values in particular ENOENT.  We may have different error returns
          // from different shards, so we'll return minimum_to_decode() error
          // (usually EIO) to reader.  It is likely an error here is due to a
          // damaged pg.
          rop.complete.at(oid).r = err;
          ++is_complete;
        }
      }

      if (!err) {
        ceph_assert(rop.complete.at(oid).r == 0);
        if (!rop.complete.at(oid).errors.empty()) {
          if (cct->_conf->osd_read_ec_check_for_errors) {
            rop.debug_log.emplace_back(ECUtil::COMPLETE_ERROR, op.from);
            dout(10) << __func__ << ": Not ignoring errors, use one shard" << dendl;
            err = rop.complete.at(oid).errors.begin()->second;
            rop.complete.at(oid).r = err;
          } else {
            get_parent()->clog_warn() << "Error(s) ignored for "
              << iter->first << " enough copies available";
            dout(10) << __func__ << " Error(s) ignored for " << iter->first
		     << " enough copies available" << dendl;
            rop.debug_log.emplace_back(ECUtil::ERROR_CLEAR, op.from);
            rop.complete.at(oid).errors.clear();
          }
        }
        // avoid re-read for completed object as we may send remaining reads for
        // uncompleted objects
        rop.to_read.at(oid).shard_reads.clear();
        rop.to_read.at(oid).want_attrs = false;
        ++is_complete;
      }
    }
  }
  if (need_resend) {
    read_pipeline.do_read_op(rop);
  } else if (rop.in_progress.empty() ||
             is_complete == rop.complete.size()) {
    dout(20) << __func__ << " Complete: " << rop << dendl;
    rop.trace.event("ec read complete");
    rop.debug_log.emplace_back(ECUtil::COMPLETE, op.from);

    /* If do_redundant_reads is set then there might be some in progress
     * reads remaining.  We need to make sure that these non-read shards
     * do not get padded. If there was no in progress read, then the zero
     * padding is allowed to stay.
     */
    for (auto pg_shard : rop.in_progress) {
      for (auto &&[oid, read] : rop.to_read) {
        read.zeros_for_decode.erase(pg_shard.shard);
      }
    }
    read_pipeline.complete_read_op(std::move(rop));
  } else {
    dout(10) << __func__ << " readop not complete: " << rop << dendl;
  }
}

void ECBackend::check_recovery_sources(const OSDMapRef &osdmap) {
  struct FinishReadOp : public GenContext<ThreadPool::TPHandle&> {
    ECCommon::ReadPipeline &read_pipeline;
    ceph_tid_t tid;

    FinishReadOp(ECCommon::ReadPipeline &read_pipeline, ceph_tid_t tid)
      : read_pipeline(read_pipeline), tid(tid) {}

    void finish(ThreadPool::TPHandle &) override {
      auto ropiter = read_pipeline.tid_to_read_map.find(tid);
      ceph_assert(ropiter != read_pipeline.tid_to_read_map.end());
      read_pipeline.complete_read_op(std::move(ropiter->second));
    }
  };
  read_pipeline.check_recovery_sources(
    osdmap,
    [this](const hobject_t &obj) {
      recovery_backend.recovery_ops.erase(obj);
    },
    [this](const ReadOp &op) {
      get_parent()->schedule_recovery_work(
        get_parent()->bless_unlocked_gencontext(
          new FinishReadOp(read_pipeline, op.tid)),
        1);
    });
}

void ECBackend::on_change() {
  reset_journal();
  rmw_pipeline.on_change();
  read_pipeline.on_change();
  rmw_pipeline.on_change2();
  clear_recovery_state();
}

void ECBackend::clear_recovery_state() {
  recovery_backend.recovery_ops.clear();
}

void ECBackend::dump_recovery_info(Formatter *f) const {
  dump_journal(f);
  f->open_array_section("recovery_ops");
  for (map<hobject_t, RecoveryBackend::RecoveryOp>::const_iterator i =
         recovery_backend.recovery_ops.begin();
       i != recovery_backend.recovery_ops.end();
       ++i) {
    f->open_object_section("op");
    i->second.dump(f);
    f->close_section();
  }
  f->close_section();
  f->open_array_section("read_ops");
  for (map<ceph_tid_t, ReadOp>::const_iterator i = read_pipeline.tid_to_read_map
                                                                .begin();
       i != read_pipeline.tid_to_read_map.end();
       ++i) {
    f->open_object_section("read_op");
    i->second.dump(f);
    f->close_section();
  }
  f->close_section();
}

struct ECClassicalOp : ECCommon::RMWPipeline::Op {
  PGTransactionUPtr t;
  bool journal_eligible = false;
  uint64_t journal_object_size = 0;
  std::optional<ObjectStore::Transaction> journal_append;
  shard_id_t journal_shard;

  void generate_transactions(
    ceph::ErasureCodeInterfaceRef &ec_impl,
    pg_t pgid,
    const ECUtil::stripe_info_t &sinfo,
    map<hobject_t, ECUtil::shard_extent_map_t> *written,
    shard_id_map<ObjectStore::Transaction> *transactions,
    DoutPrefixProvider *dpp,
    const OSDMapRef &osdmap,
    bool& first_write_in_interval) final {
    ceph_assert(t);
    // Keep OI current on all shards: journal writes have no data write set from
    // which the usual partial-write path could infer shard versions.
    bool write_all = journal_append ? true : first_write_in_interval;
    ECTransaction::generate_transactions(
      t.get(),
      plan,
      ec_impl,
      pgid,
      sinfo,
      remote_shard_extent_map,
      log_entries,
      written,
      transactions,
      &temp_added,
      &temp_cleared,
      dpp,
      osdmap,
      write_all);
    first_write_in_interval = write_all;
    if (journal_append) {
      // Only the primary stores the payload; the normal commit gather waits
      // for this transaction and the remote metadata/log transactions.
      transactions->at(journal_shard).append(*journal_append);
      journal_append.reset();
    }
  }

  bool skip_transaction(
      std::set<shard_id_t> &pending_roll_forward,
      shard_id_t shard,
      ceph::os::Transaction &transaction) final {
    if (transaction.empty()) {
      return true;
    }
    pending_roll_forward.insert(shard);
    return false;
  }
};

// No PG log entries or metadata updates: these writes materialize data already
// acknowledged from the journal. Cache ordering and shard commit gathering are
// still provided by the normal RMW pipeline.
struct ECJournalFlushOp final : ECCommon::RMWPipeline::Op {
  ECWriteJournal::Stripe stripe;

  void generate_transactions(
      ErasureCodeInterfaceRef& ec_impl, pg_t pgid,
      const ECUtil::stripe_info_t& sinfo,
      map<hobject_t, ECUtil::shard_extent_map_t>* written,
      shard_id_map<ObjectStore::Transaction>* transactions,
      DoutPrefixProvider* dpp, const OSDMapRef&,
      bool&) final {
    ECUtil::shard_extent_map_t empty(&sinfo);
    auto read = remote_shard_extent_map.find(hoid);
    auto data = ECWriteJournal::assemble_flush(stripe, sinfo,
      read == remote_shard_extent_map.end() ? empty : read->second);
    data.insert_parity_buffers();
    ceph_assert(data.encode(ec_impl, dpp) == 0);
    const uint64_t chunk_off = stripe.offset / sinfo.get_k();
    for (auto& [shard, txn] : *transactions) {
      bufferlist chunk;
      data.get_buffer(shard, chunk_off, sinfo.get_chunk_size(), chunk);
      ceph_assert(chunk.length() == sinfo.get_chunk_size());
      txn.write(coll_t(spg_t(pgid, shard)),
        ghobject_t(hoid, ghobject_t::NO_GEN, shard), chunk_off,
        chunk.length(), chunk);
    }
    written->emplace(hoid, std::move(data));
  }

  bool skip_transaction(std::set<shard_id_t>&,
                        shard_id_t, ObjectStore::Transaction& t) final {
    return t.empty();
  }
};

ECBackend::~ECBackend() = default;

void ECBackend::init_journal()
{
  if (journal_state ||
      !cct->_conf.get_val<bool>("osd_ec_journal_poc_enable") ||
      cct->_conf.get_val<int64_t>("osd_ec_journal_poc_pool") !=
        get_parent()->get_info().pgid.pgid.pool()) {
    return;
  }
  ceph_assert((sinfo.get_k() == 8 || sinfo.get_k() == 12) &&
              sinfo.get_m() == 3 && sinfo.get_chunk_size() == 4096);
  ECWriteJournal::Limits limits;
  limits.stripe_width = sinfo.get_k() * sinfo.get_chunk_size();
  limits.segment_bytes = cct->_conf.get_val<uint64_t>("osd_ec_journal_poc_segment_bytes");
  limits.max_bytes = cct->_conf.get_val<uint64_t>("osd_ec_journal_poc_max_bytes");
  limits.max_records = cct->_conf.get_val<uint64_t>("osd_ec_journal_poc_max_records");
  limits.max_segments = std::max(uint64_t(1), limits.max_bytes / limits.segment_bytes);
  const auto pgid = get_parent()->get_info().pgid.pgid;
  // Temp-pool objects cannot collide with client names. Do not register these
  // in temp_contents (peering deletes those objects). There is no replay yet.
  const hobject_t anchor(object_t("ec-write-journal"), "", CEPH_NOSNAP,
                         pgid.ps(), pgid.pool(), "");
  auto name = std::string("ec-write-journal.") +
    std::to_string(ceph::real_clock::now().time_since_epoch().count()) + "." +
    std::to_string(get_parent()->get_tid());
  ghobject_t prefix(anchor.make_temp_hobject(name), ghobject_t::NO_GEN,
                     get_parent()->whoami_shard().shard);
  journal_state = std::make_unique<JournalState>(this, std::move(prefix), limits,
    cct->_conf.get_val<uint64_t>("osd_ec_journal_poc_flush_ms"));
  dout(0) << "UNSAFE write-only EC journal POC enabled for " << pgid
          << "; primary-local data, no replay/recovery or dirty reads" << dendl;
}

void ECBackend::schedule_journal(std::chrono::milliseconds delay)
{
  auto& state = *journal_state;
  const auto when = ceph::mono_clock::now() + delay;
  if (state.timer.is_scheduled()) {
    if (state.wake_at <= when) {
      return;
    }
    get_parent()->get_pg_timer().cancel(state.timer);
  }
  // Timer invocation acquires the PG lock, so even zero delay cannot reenter
  // RMWPipeline::finish_rmw recursively. Commit callbacks preempt the timeout.
  state.wake_at = when;
  get_parent()->get_pg_timer().schedule_after(state.timer, delay);
}

void ECBackend::reset_journal()
{
  if (!journal_state) {
    return;
  }
  if (journal_state->timer.is_scheduled()) {
    get_parent()->get_pg_timer().cancel(journal_state->timer);
  }
  ceph_assertf(journal_state->journal.empty(),
    "EC journal POC: PG reset with dirty records; replay is NOT implemented");
  journal_state->pending.clear();
  // Do not destroy the timer here: its thread may be releasing a reference
  // after cancellation. Its lifetime is the backend's lifetime.
}

void ECBackend::dump_journal(Formatter* f) const
{
  if (!journal_state) {
    return;
  }
  const auto& s = *journal_state;
  f->open_object_section("ec_write_journal_poc");
  f->dump_unsigned("bytes", s.journal.bytes());
  f->dump_unsigned("records", s.journal.records());
  f->dump_unsigned("pending_admissions", s.pending.size());
  f->dump_bool("flush_in_progress", bool(s.flush));
  f->dump_unsigned("admitted", s.admitted);
  f->dump_unsigned("acked", s.acked);
  f->dump_unsigned("full_stripes", s.full_stripes);
  f->dump_unsigned("partial_stripes", s.partial_stripes);
  f->dump_unsigned("planned_flush_read_bytes", s.planned_read_bytes);
  f->dump_unsigned("flushed_logical_bytes", s.flushed_bytes);
  f->dump_unsigned("pressure_events", s.pressure_events);
  f->close_section();
}

void ECBackend::pump_journal()
{
  auto& s = *journal_state;
  ceph_assert(!s.pumping);
  s.pumping = true;
  const bool healthy = !get_parent()->pg_is_undersized() &&
    get_parent()->get_acting_shards().size() == sinfo.get_k_plus_m() &&
    get_parent()->get_backfill_shards().empty();
  ceph_assertf(healthy || s.journal.empty(),
    "EC journal POC requires a fixed healthy acting set while dirty");
  const bool drain = cct->_conf.get_val<bool>("osd_ec_journal_poc_drain");
  if (drain || ceph::mono_clock::now() - s.open_since >=
                 std::chrono::milliseconds(s.flush_ms)) {
    s.journal.seal();
  }

  while (!s.pending.empty()) {
    auto op = s.pending.front();
    if (!op->journal_eligible || drain || !healthy) {
      // Unsupported mutations cannot overtake buffered data.
      if (!s.journal.empty()) {
        s.journal.seal();
        break;
      }
      s.pending.pop_front();
      start_transaction(std::move(op));
      continue;
    }
    auto& update = op->t->op_map.at(op->hoid);
    const auto extent = update.buffer_updates.begin();
    const auto& write = boost::get<PGTransaction::ObjectOperation::BufferUpdate::Write>(
      extent.get_val());
    ObjectStore::Transaction append;
    ECWriteJournal::Ticket ticket;
    int r = s.journal.append(op->hoid, op->version, op->journal_object_size,
                            extent.get_off(), write.buffer, append, &ticket);
    if (r == -EAGAIN) {
      ++s.pressure_events;
      s.journal.seal();
      break; // requests stay unacknowledged under the OSD's admission limits
    }
    ceph_assert(r == 0);
    if (s.open_segment != ticket.segment) {
      s.open_segment = ticket.segment;
      s.open_since = ceph::mono_clock::now();
      s.coverage.clear();
    }
    const uint64_t width = sinfo.get_k() * sinfo.get_chunk_size();
    const uint64_t start = extent.get_off() / width * width;
    auto& coverage = s.coverage[{op->hoid, start}];
    coverage.union_insert(extent.get_off() - start, write.buffer.length());
    if (coverage.size() == std::min(width, op->journal_object_size - start)) {
      s.journal.seal();
    }
    update.buffer_updates.clear(); // BEFORE get_write_plan(), hence no RMW
    op->journal_append.emplace(std::move(append));
    op->journal_shard = get_parent()->whoami_shard().shard;
    auto completion = std::unique_ptr<Context>(std::exchange(op->on_all_commit, nullptr));
    op->on_all_commit = make_lambda_context(
      [this, ticket, completion = std::move(completion)](int result) mutable {
        ceph_assert(result == 0);
        ceph_assert(journal_state->journal.committed(ticket, result) == 0);
        ++journal_state->acked;
        completion.release()->complete(result);
        schedule_journal();
      });
    ++s.admitted;
    s.pending.pop_front();
    start_transaction(std::move(op));
  }

  if (!s.flush) {
    s.flush = s.journal.begin_flush();
    s.next_stripe = 0;
  }
  if (s.flush && !s.flush_in_progress && !s.trimming) {
    if (s.next_stripe == s.flush->stripes.size()) {
      ObjectStore::Transaction trim;
      const auto segment = s.flush->segment;
      ceph_assert(s.journal.finish_flush(segment, 0, trim) == 0);
      s.trimming = true;
      trim.register_on_commit(get_parent()->bless_context(make_lambda_context(
        [this, segment](int r) {
          ceph_assert(r == 0);
          auto& s = *journal_state;
          ceph_assert(s.journal.trimmed(segment, r) == 0);
          s.trimming = false;
          s.flush.reset();
          if (s.journal.empty()) {
            s.coverage.clear();
          }
          schedule_journal();
        })));
      get_parent()->queue_transaction(std::move(trim));
    } else {
      auto op = std::make_shared<ECJournalFlushOp>();
      op->stripe = s.flush->stripes[s.next_stripe];
      op->hoid = op->stripe.object;
      op->tid = get_parent()->get_tid();
      op->pipeline = &rmw_pipeline;
      // Zero version: do not apply/log the foreground operation a second time.
      op->version = eversion_t();
      op->pg_committed_to = rmw_pipeline.committed_to;
      const auto shards = read_pipeline.get_readable_writable_shard_id_sets().first;
      auto plan = ECWriteJournal::plan_flush(op->stripe, sinfo, shards);
      if (plan.to_read) {
        op->plan.want_read = true;
        for (const auto& [shard, extents] : *plan.to_read) {
          s.planned_read_bytes += extents.size();
        }
      }
      op->plan.plans.push_back(std::move(plan));
      s.flush_in_progress = true;
      const uint64_t width = op->stripe.width;
      const bool full = op->stripe.full();
      // Called by finish_rmw with the PG lock held, like C_OSD_RepopCommit.
      // Do not bless again here (that would recursively lock the PG mutex).
      op->on_all_commit = make_lambda_context([this, width, full](int r) {
        ceph_assert(r == 0);
        auto& s = *journal_state;
        s.flush_in_progress = false;
        ++s.next_stripe;
        (full ? s.full_stripes : s.partial_stripes)++;
        s.flushed_bytes += width;
        schedule_journal();
      });
      rmw_pipeline.start_rmw(std::move(op));
    }
  }
  s.pumping = false;
  const auto remaining = s.open_since + std::chrono::milliseconds(s.flush_ms) -
                         ceph::mono_clock::now();
  if (!s.journal.empty() && remaining > decltype(remaining)::zero()) {
    // No periodic polling while waiting for append/flush/trim commits: each
    // completion schedules progress. This timer only expires the open segment.
    schedule_journal(std::chrono::ceil<std::chrono::milliseconds>(remaining));
  }
}

void ECBackend::start_transaction(std::shared_ptr<ECClassicalOp> op)
{
  op->plan = get_write_plan(sinfo, *op->t, read_pipeline, rmw_pipeline,
                           get_parent()->get_dpp());
  ldpp_dout(get_parent()->get_dpp(), 20) << __func__
    << " plans=" << op->plan << dendl;
  rmw_pipeline.start_rmw(std::move(op));
}

std::tuple<
  int,
  map<string, bufferlist, less<>>,
  size_t
> ECBackend::get_attrs_n_size_from_disk(const hobject_t &hoid) {
  struct stat st;
  if (int r = object_stat(hoid, &st); r < 0) {
    dout(10) << __func__ << ": stat error " << r << " on" << hoid << dendl;
    return {r, {}, 0};
  }
  map<string, bufferlist, less<>> real_attrs;
  if (int r = switcher->objects_get_attrs_with_hinfo(hoid, &real_attrs); r < 0) {
    dout(10) << __func__ << ": get attr error " << r << " on" << hoid << dendl;
    return {r, {}, 0};
  }
  return {0, real_attrs, st.st_size};
}

void ECBackend::submit_transaction(
  const hobject_t &hoid,
  const object_stat_sum_t &delta_stats,
  const eversion_t &at_version,
  PGTransactionUPtr &&t,
  const eversion_t &trim_to,
  const eversion_t &pg_committed_to,
  vector<pg_log_entry_t> &&log_entries,
  std::optional<pg_hit_set_history_t> &hset_history,
  Context *on_all_commit,
  ceph_tid_t tid,
  osd_reqid_t reqid,
  OpRequestRef client_op
) {
  auto op = std::make_shared<ECClassicalOp>();
  auto obc_map = t->obc_map;
  op->t = std::move(t);
  op->hoid = hoid;
  op->delta_stats = delta_stats;
  op->version = at_version;
  op->trim_to = trim_to;
  /* We update PeeringState::pg_committed_to via the callback
   * invoked from ECBackend::handle_sub_write_reply immediately
   * before updating rmw_pipeline.commited_to via
   * rmw_pipeline.check_ops()->finish_rmw(), so these will
   * *usually* match.  However, the PrimaryLogPG::submit_log_entries
   * pathway can perform an out-of-band log update which updates
   * PeeringState::pg_committed_to independently.  Thus, the value
   * passed in is the right one to use. */
  op->pg_committed_to = pg_committed_to;
  op->log_entries = log_entries;
  std::swap(op->updated_hit_set_history, hset_history);
  op->on_all_commit = on_all_commit;
  op->tid = tid;
  op->reqid = reqid;
  op->client_op = client_op;
  op->pipeline = &rmw_pipeline;
  if (client_op) {
    op->trace = client_op->pg_trace;
  }
  init_journal();
  if (!journal_state) {
    start_transaction(std::move(op));
    return;
  }
  // One ordinary overwrite with only the normal OI attribute update. Capture
  // its size now, not from the potentially newer OBC when it eventually drains.
  if (hoid.snap == CEPH_NOSNAP && op->t->op_map.size() == 1 &&
      op->t->op_map.contains(hoid) && op->log_entries.size() == 1 &&
      op->log_entries.front().soid == hoid && op->t->obc_map.contains(hoid)) {
    const auto& update = op->t->op_map.at(hoid);
    auto old_oi = get_object_info_from_obc(op->t->obc_map.at(hoid));
    if (update.is_none() && !update.truncate && !update.updated_snaps &&
        !update.alloc_hint && !update.clear_omap && !update.omap_header &&
        update.omap_updates.empty() && update.attr_updates.size() == 1 &&
        update.attr_updates.contains(OI_ATTR) && update.attr_updates.at(OI_ATTR) &&
        old_oi && !update.buffer_updates.empty()) {
      const object_info_t oi(*update.attr_updates.at(OI_ATTR));
      auto i = update.buffer_updates.begin();
      const auto* w = boost::get<PGTransaction::ObjectOperation::BufferUpdate::Write>(
        &i.get_val());
      const uint64_t off = i.get_off();
      const uint64_t len = i.get_len();
      const uint64_t width = sinfo.get_k() * sinfo.get_chunk_size();
      if (++i == update.buffer_updates.end() && w &&
          (len == 1024 || len == 4096) && off % 1024 == 0 &&
          oi.size == old_oi->size && off <= oi.size && len <= oi.size - off &&
          len <= width - off % width) {
        op->journal_eligible = true;
        op->journal_object_size = oi.size;
      }
    }
  }
  journal_state->pending.push_back(std::move(op));
  pump_journal();
}

int ECBackend::objects_read_sync(
    const hobject_t &hoid,
    uint64_t off,
    uint64_t len,
    uint32_t op_flags,
    bufferlist *bl) {
  return -EOPNOTSUPP;
}

void ECBackend::objects_read_async(
    const hobject_t &hoid,
    uint64_t object_size,
    const list<pair<ec_align_t,
                    pair<bufferlist*, Context*>>> &to_read,
    Context *on_complete,
    bool fast_read) {
  map<hobject_t, std::list<ec_align_t>> reads;

  uint32_t flags = 0;
  extent_set es;
  for (const auto &[read, ctx]: to_read) {
    pair<uint64_t, uint64_t> tmp;
    if (!cct->_conf->osd_ec_partial_reads) {
      tmp = sinfo.ro_offset_len_to_stripe_ro_offset_len(read.offset, read.size);
    } else {
      tmp.first = read.offset;
      tmp.second = read.size;
    }
    es.union_insert(tmp.first, tmp.second);
    flags |= read.flags;
  }

  if (!es.empty()) {
    auto &offsets = reads[hoid];
    for (auto [off, len]: es) {
      offsets.emplace_back(ec_align_t{off, len, flags});
    }
  }

  struct cb {
    ECBackend *ec;
    hobject_t hoid;
    list<pair<ec_align_t,
              pair<bufferlist*, Context*>>> to_read;
    unique_ptr<Context> on_complete;
    cb(const cb &) = delete;
    cb(cb &&) = default;

    cb(ECBackend *ec,
       const hobject_t &hoid,
       const list<pair<ec_align_t,
                       pair<bufferlist*, Context*>>> &to_read,
       Context *on_complete)
      : ec(ec),
        hoid(hoid),
        to_read(to_read),
        on_complete(on_complete) {}

    void operator()(ECCommon::ec_extents_t &&results) {
      auto dpp = ec->get_parent()->get_dpp();
      ldpp_dout(dpp, 20) << "objects_read_async_cb: got: " << results
			 << dendl;

      auto &got = results.at(hoid);

      int r = 0;
      for (auto &&[read, result]: to_read) {
        auto &&[bufs, ctx] = result;
        if (got.err < 0) {
          // error handling
          if (ctx) {
            ctx->complete(got.err);
          }
          if (r == 0)
            r = got.err;
        } else {
          ceph_assert(bufs);
          uint64_t offset = read.offset;
          uint64_t length = read.size;
          auto range = got.emap.get_containing_range(offset, length);
          uint64_t range_offset = range.first.get_off();
          uint64_t range_length = range.first.get_len();
          ceph_assert(range.first != range.second);
          ceph_assert(range_offset <= offset);
          ldpp_dout(dpp, 20) << "offset: " << offset << dendl;
          ldpp_dout(dpp, 20) << "range offset: " << range_offset << dendl;
          ldpp_dout(dpp, 20) << "length: " << length << dendl;
          ldpp_dout(dpp, 20) << "range length: " << range_length << dendl;
          ceph_assert((offset + length) <= (range_offset + range_length));
          bufs->substr_of(
            range.first.get_val(),
            offset - range_offset,
            length);
          if (ctx) {
            ctx->complete(length);
            ctx = nullptr;
          }
        }
      }
      to_read.clear();
      if (on_complete) {
        on_complete.release()->complete(r);
      }
    }

    ~cb() {
      for (auto &&i: to_read) {
        delete i.second.second;
      }
      to_read.clear();
    }
  };
  objects_read_and_reconstruct(
    reads,
    fast_read,
    object_size,
    make_gen_lambda_context<
      ECCommon::ec_extents_t&&, cb>(
      cb(this,
         hoid,
         to_read,
         on_complete)));
}

void ECBackend::objects_read_and_reconstruct(
  const map<hobject_t, std::list<ec_align_t>> &reads,
  bool fast_read,
  uint64_t object_size,
  GenContextURef<ECCommon::ec_extents_t&&> &&func) {
  return read_pipeline.objects_read_and_reconstruct(
    reads, fast_read, object_size, std::move(func));
}

void ECBackend::objects_read_and_reconstruct_for_rmw(
  map<hobject_t, read_request_t> &&to_read,
  GenContextURef<ECCommon::ec_extents_t&&> &&func) {
  return read_pipeline.objects_read_and_reconstruct_for_rmw(
    std::move(to_read), std::move(func));
}

void ECBackend::kick_reads() {
  read_pipeline.kick_reads();
}

int ECBackend::object_stat(
  const hobject_t &hoid,
  struct stat *st) {
  int r = switcher->store->stat(
    switcher->ch,
    ghobject_t{hoid, ghobject_t::NO_GEN, get_parent()->whoami_shard().shard},
    st);
  return r;
}

int ECBackend::objects_get_attrs(
  const hobject_t &hoid,
  map<string, bufferlist, less<>> *out) {
  for (map<string, bufferlist>::iterator i = out->begin();
       i != out->end();
  ) {
    if (ECUtil::is_hinfo_key_string(i->first))
      out->erase(i++);
    else
      ++i;
  }
  return 0;
}

int ECBackend::be_deep_scrub(
  const Scrub::ScrubCounterSet& io_counters,
  const hobject_t &poid,
  ScrubMap &map,
  ScrubMapBuilder &pos,
  ScrubMap::object &o) {
  dout(10) << __func__ << " " << poid << " pos " << pos << dendl;
  int r;

  uint32_t fadvise_flags = CEPH_OSD_OP_FLAG_FADVISE_SEQUENTIAL |
    CEPH_OSD_OP_FLAG_FADVISE_DONTNEED |
    CEPH_OSD_OP_FLAG_BYPASS_CLEAN_CACHE;

  utime_t sleeptime;
  sleeptime.set_from_double(cct->_conf->osd_debug_deep_scrub_sleep);
  if (sleeptime != utime_t()) {
    lgeneric_derr(cct) << __func__ << " sleeping for " << sleeptime << dendl;
    sleeptime.sleep();
  }

  if (pos.data_pos == 0) {
    pos.data_hash = bufferhash(-1);
  }

  uint64_t stride = cct->_conf->osd_deep_scrub_stride;
  if (stride % sinfo.get_chunk_size())
    stride += sinfo.get_chunk_size() - (stride % sinfo.get_chunk_size());

  auto& perf_logger = *(get_parent()->get_logger());
  perf_logger.inc(io_counters.read_cnt);
  bufferlist bl;
  r = switcher->store->read(
    switcher->ch,
    ghobject_t(
      poid, ghobject_t::NO_GEN, get_parent()->whoami_shard().shard),
    pos.data_pos,
    stride, bl,
    fadvise_flags);
  if (r < 0) {
    dout(20) << __func__ << "  " << poid << " got "
	     << r << " on read, read_error" << dendl;
    o.read_error = true;
    return 0;
  }
  if (r > 0) {
    pos.data_hash << bl;
  }
  perf_logger.inc(io_counters.read_bytes, r);
  pos.data_pos += r;
  if (r == (int)stride) {
    return -EINPROGRESS;
  }

  o.digest = 0;
  o.digest_present = true;
  o.omap_digest = -1;
  o.omap_digest_present = true;
  return 0;
}
