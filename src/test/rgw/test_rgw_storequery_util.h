/**
 * @file test_rgw_storequery_util.h
 * @author André Lucas (alucas@akamai.com)
 * @brief Utility functions for testing storequery.
 * @version 0.1
 * @date 2025-08-21
 *
 * @copyright Copyright (c) 2025
 *
 */

#include <set>
#include <string>

#include "cls/rgw/cls_rgw_types.h"
#include "rgw_sal.h"

namespace storequery_util {

using SALBucket = rgw::sal::Bucket;

// Minimally model an entry in a bucket index. There's a lot we don't care
// about, but we do care about the key's name, instance, and versioning.
struct SrcKey {
  std::string name = "";
  std::string instance = "";
  bool versioned = false;
  bool exists = false;
  bool current = false;
  bool delete_marker = false;

  enum Flag {
    /// The VERSIONED flag must be present for the CURRENT flag to take
    /// effect, namely for rgw_bucket_dir_entry.is_versioned() to operate.
    VERSIONED = 1 << 0,
    EXISTS = 1 << 1,
    CURRENT = 1 << 2,
    DELETE_MARKER = 1 << 3
  };

  /// Construct a nonversioned object, which exists and is current.
  SrcKey(const std::string& name)
      : name(name)
      , versioned(false)
      , exists(true)
      , current(true)
  {
  }
  /// Construct a versioned object, which exists and is current, but is not a
  /// delete marker.
  SrcKey(const std::string& name, const std::string& instance)
      : name(name)
      , instance(instance)
      , versioned(true)
      , exists(true)
      , current(true)
  {
  }
  /// Construct a potentially versioned object, setting the versioned, exists,
  /// current and delete_marker flags specifically.
  SrcKey(const std::string& name, const std::string& instance, int flags)
      : name(name)
      , instance(instance)
  {
    versioned = (flags & VERSIONED) != 0;
    exists = (flags & EXISTS) != 0;
    current = (flags & CURRENT) != 0;
    delete_marker = (flags & DELETE_MARKER) != 0;
  }

  // Default auto-generated constructors are all fine.

  // Create a new delete marker suitable for the top of the version stack,
  // i.e. current, !exists, delete_marker.
  SrcKey new_delete_marker(const std::string& name, const std::string& instance)
  {
    return SrcKey(name, instance, Flag::VERSIONED | Flag::CURRENT | Flag::DELETE_MARKER);
  }

  /// Mimic somewhat the behaviour of rgw_bucket_dir_entry.is_current().
  bool is_current() const
  {
    return (!versioned) || (versioned && current && !delete_marker);
  }

  rgw_bucket_dir_entry to_dir_entry() const
  {
    rgw_bucket_dir_entry entry;
    entry.key = cls_rgw_obj_key(name, instance);
    entry.exists = exists;

    uint16_t flags = 0;
    if (versioned) {
      flags |= rgw_bucket_dir_entry::FLAG_VER;
    }
    if (current) {
      flags |= rgw_bucket_dir_entry::FLAG_CURRENT;
    }
    if (delete_marker) {
      flags |= rgw_bucket_dir_entry::FLAG_DELETE_MARKER;
    }
    entry.flags = flags;
    return entry;
  }

  rgw_obj_key to_marker() const
  {
    return rgw_obj_key(name, instance);
  }

  // Let the compiler create relational operators for us, since we're C++20.
  std::strong_ordering operator<=>(const SrcKey& other) const
  {
    if (auto cmp = name <=> other.name; cmp != 0) {
      return cmp;
    }
    return instance <=> other.instance;
  }

  // Detailed == operator. Don't use this when comparing just key names and
  // instance IDs, it won't work the way you want unless you're lucky.
  bool operator==(const SrcKey& other) const
  {
    return name == other.name
        && instance == other.instance
        && versioned == other.versioned
        && exists == other.exists
        && current == other.current
        && delete_marker == other.delete_marker;
  }

  std::string to_string() const
  {
    return fmt::format(FMT_STRING("SrcKey(name={}, instance={}, versioned={}, exists={}, current={}, delete_marker={})"),
        name, instance, versioned, exists, current, delete_marker);
  }

  friend std::ostream& operator<<(std::ostream& os, const SrcKey& key)
  {
    os << key.to_string();
    return os;
  }

}; // struct SrcKey

/**
 * @brief Store some object keys and attempt to mimic rgw::sal::Bucket's
 * interface for querying a bucket. Provide list implementations suitable for
 * direct substitution in storequery's objectlist implementation, so unit
 * tests can be written.
 */
class BucketDirSim {

public:
  using bucket_type = std::vector<SrcKey>;

private:
  bucket_type src_bucket_;

public:
  /// Move the provided bucket into place, resetting the donor bucket.
  void set_bucket(bucket_type&& bucket)
  {
    src_bucket_ = std::move(bucket);
  }

  /// Return a const ref to the simulated bucket.
  const bucket_type& get_bucket() const
  {
    return src_bucket_;
  }

  /// Reset the bucket and fill with \p count items named `objDDDD` where DDDD
  /// is the zero-padded object index, starting at 0000.
  void fill_bucket_nonversioned(size_t count)
  {
    std::vector<SrcKey> src;
    src.reserve(count);
    for (int i = 0; i < count; ++i) {
      // This uses the single-string (nonversioned object) constructor.
      src.emplace_back(fmt::format("obj{:04d}", i));
    }
    set_bucket(std::move(src));
  }

   /**
    * @brief  Reset the bucket and fill with \p versions_per_item versions of
    * \p count items.
    *
    * Items are named `objDDDD` where DDDD is the zero-padded object index,
    * starting at 0000. Items are versioned `vDDDD`, where DDDD is the
    * zero-padded version starting at 0000.
    *
    * The first (lowest-versioned) version is set to 'current'. This is in
    * keeping with the order that RGW normally returns objects, with the
    * current version first.
    *
    * \p deletions is a list of object indexes (starting at 0) to delete. If
    * an index is to be deleted, none of the regular (non-delete marker)
    * versions will be marked as current, and instead a delete marker will be
    * inserted (before the versions) and the delete marker will be marked
    * current. This again is how RGW reports it.
    *
    * There's no current provision for delete markers not at the 'top' of the
    * history stack. You'd have to do that manually, and carefully set the
    * current markers yourself.
    *
    * @param count The number of object key names.
    * @param versions_per_item The number of versions of each object key name.
    * @param deletions A vector of item indices to mark as deleted.
    */
  void fill_bucket_versioned(size_t count, size_t versions_per_item, std::vector<size_t> deletions = {})
  {
    std::vector<bool> del(count);
    assert(del.size() == count);
    for (auto index : deletions) {
      // Allow for a fixed array of deletions that might be larger than count.
      if (index < del.size()) {
        del[index] = true;
      }
    }

    std::vector<SrcKey> src;
    src.reserve(count * versions_per_item);
    for (int i = 0; i < count; i++) {
      // rados will put the current entry first.
      if (del[i]) {
        SrcKey newkey { fmt::format("obj{:04d}", i), "d0001", SrcKey::Flag::VERSIONED | SrcKey::Flag::CURRENT | SrcKey::Flag::DELETE_MARKER };
        // ldpp_dout(dpp, 20) << fmt::format(
        //     FMT_STRING("fill_bucket_versioned() adding delete marker key: {}"), newkey.to_string())
        //                    << dendl;
        src.push_back(newkey);
      }
      for (int v = 0; v < versions_per_item; v++) {
        bool current = false;
        if (del[i]) {
          current = false;
        } else {
          // Current entry goes first.
          current = (v == 0);
        }
        // This uses the full constructor (name, instance, flags).
        using f = SrcKey::Flag;
        SrcKey newkey { fmt::format("obj{:04d}", i), fmt::format("v{:04d}", v), f::VERSIONED | (current ? f::CURRENT : 0) | f::EXISTS };
        // ldpp_dout(dpp, 20) << fmt::format(
        //     FMT_STRING("fill_bucket_versioned() adding key: {}"), newkey.to_string())
        //                    << dendl;
        src.push_back(newkey);
      }
    }
    set_bucket(std::move(src));
  }

  /// Return a set populated with all object keys. Note this includes deleted
  /// keys.
  std::set<std::string> bucket_object_keys()
  {
    std::set<std::string> res;
    for (const auto& key : get_bucket()) {
      res.insert(key.name);
    }
    return res;
  }

public:
  /**
   * @brief Clear the results of a bucket list operation. Expected to be
   * called by list implementations to set \p results to sane default values.
   *
   * @param results The results Object to clear.
   */
  void clear_results(SALBucket::ListResults& results)
  {
    results.objs.clear();
    results.common_prefixes.clear();
    results.next_marker = rgw_obj_key();
    results.is_truncated = false;
  }
  /// List implementation that always throws an exception.
  int list_always_throw(const DoutPrefixProvider* dpp, SALBucket::ListParams&, int, SALBucket::ListResults& results, optional_yield y)
  {
    clear_results(results);
    throw std::runtime_error("list_throw");
  }

  /// List implementation that always returns -ERR_INTERNAL_ERROR.
  int list_always_fail(const DoutPrefixProvider* dpp, SALBucket::ListParams&, int, SALBucket::ListResults& results, optional_yield y)
  {
    clear_results(results);
    return -ERR_INTERNAL_ERROR;
  }

  /**
   * @brief List implementation that always returns an empty list and EoL.
   *
   * 'Empty' means the items() list is empty. EoL means that 'is_truncated' is
   * set in \p results.
   *
   * @param dpp     DoutPrefixProvider for logging.
   * @param param   Parameters to the list function.
   * @param results InOut: Results from the simulated list function.
   * @param y       Optional yield object.
   * @return int    The error code. Zero means success, <0 means failure with
   * a meaningful code.
   */
  int list_always_empty(const DoutPrefixProvider* dpp, SALBucket::ListParams& param, int, SALBucket::ListResults& results, optional_yield y)
  {
    clear_results(results);
    results.is_truncated = false;
    return 0; // Success.
  }

  /**
   * @brief Mimic the standard SAL list operation.
   *
   * Take the src_bucket_ array and page it out to the results object as
   * list() would.
   *
   * @param dpp The DoutPrefixProvider for logging.
   * @param param Parameters to the list function.
   * @param results InOut: Results from the simulated list function.
   * @param y       Optional yield object.
   * @return int    The error code. Zero means success, <0 means failure with
   * a meaningful code.
   */
  int list_standard(const DoutPrefixProvider* dpp, SALBucket::ListParams& param, int max_entries, SALBucket::ListResults& results, optional_yield y)
  {
    clear_results(results);

    auto& objs = results.objs;

    size_t start_index = 0;
    if (!param.marker.empty()) {
      bool seen_marker = false;

      // Create a search key for comparison. We'll start in lexicographical
      // order, which is how rgw::sal::Bucket::list() appears to work.

      SrcKey search = SrcKey(param.marker.name, param.marker.instance);

      // If there's a marker, scan the bucket for the marker. Note we don't
      // include the last item, a token pointing at the last item doesn't make
      // sense (we shouldn't have returned it, it's the EOF).
      for (size_t n = 0; n < get_bucket().size() - 1; ++n) {
        SrcKey entry = get_bucket()[n];
        if (search <= entry) {

          // It matters if we matched exactly. If we did, we want to start at
          // the following item, otherwise start at the existing item.
          if (search.name == entry.name && search.instance == entry.instance) {
            start_index = n + 1;
          } else {
            start_index = n;
          }
          seen_marker = true;
          break;
        }
      }
      if (!seen_marker) {
        ldpp_dout(dpp, 20) << fmt::format(FMT_STRING("marker[name={},instance={}] not found in bucket"), param.marker.name, param.marker.instance) << dendl;
        results.is_truncated = true;
        return -ENOENT; // XXX XXX what does RGW do with marker-not-found?
      }
    }
    ldpp_dout(dpp, 20) << fmt::format(
        FMT_STRING("list_standard() marker=[name={},instance={}] start_index={}"),
        param.marker.name, param.marker.instance, start_index)
                       << dendl;

    bool seen_eof = false;

    size_t n = start_index;
    for (; objs.size() < max_entries && n < get_bucket().size(); n++) {
      auto src_obj = get_bucket()[n];
      auto entry = src_obj.to_dir_entry();
      objs.push_back(entry);
      // Just keep this as a running item, to simplify bookkeeping.
      results.next_marker = src_obj.to_marker();
      // Likewise update the params, as the API expects.
      param.marker = results.next_marker;
    }
    ldpp_dout(dpp, 5) << fmt::format(
        FMT_STRING("list_standard() loop exit n={} objs.size={} marker=[name={},instance={}]"),
        n, objs.size(), param.marker.name, param.marker.instance)
                      << dendl;
    assert(objs.size() <= max_entries);
    if (n == get_bucket().size()) {
      seen_eof = true;
      // In this case, we don't want a marker set.
      results.next_marker = rgw_obj_key();
    }
    // is_truncated means there are more results available.
    results.is_truncated = !seen_eof;

    return 0; // Success.
  }

}; // class BucketDirSim

} // namespace storequery_util
