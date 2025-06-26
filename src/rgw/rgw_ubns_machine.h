/**
 * @file rgw_ubns_machine.h
 * @author André Lucas (alucas@akamai.com)
 * @brief UBNS state machines templates.
 * @version 0.1
 * @date 2024-04-15
 *
 * @copyright Copyright (c) 2024
 *
 */

#pragma once

/*
 * Don't include this except in .cc files where it's actually needed! There's
 * literally no need to have hundreds of files include a template which they
 * have to check only to discard.
 */

#include "common/dout.h"
#include "rgw_ubns.h"
#include <rgw_common.h>

namespace rgw {

/**
 * @brief State machine implementing the client Create side of the UBNS
 * protocol.
 *
 * This state machine is used to implement the two-phase commit protocol for
 * UBNS bucket creation. The formality is so that we can create a RAII object
 * that does the proper rollback regardless of the method by which
 * RGWCreateBucket::execute() is exited.
 *
 * (The majority of exit points from RGWCreateBucket::execute() are error
 * exits; manually cleaning up the UBNS state in each would be messy,
 * error-prone and hard-to-maintain.)
 *
 * The state machine is implemented as a template so we can instantiate a
 * non-gRPC version in the unit tests and test the machine completely
 * standalone.
 *
 * The machine itself is very simple. Given three gRPC services, one for the
 * initial Bucket Create request, one to Update the bucket to the 'created'
 * state once the bucket has really been created, and one to Delete the bucket
 * if the creation process fails, then the machine manages the sequencing of
 * the calls and any rollback.
 *
 * (One slight wrinkle is the need to support 'idempotent recreate'. If the
 * user attempts to recreate a bucket that they already own, that attempt
 * should appear succeed without error. This doesn't mean we don't have to
 * handle this as a special case though - we don't want to go through the
 * two-stage commit for buckets in this instance, we just want RGW processing
 * to continue as normal. RGW will detect that the bucket already exists and
 * return an OK response. We, however, don't want to sent our Update message
 * under any circumstances, as it will fail. To achieve this, we set a special
 * 'soft failure' state that tells the machine to skip the Update RPC. This
 * works whether we call set_state() manually as part of normal processing, or
 * whether we call set_state() automatically in the destructor.
 *
 * It's necessary to have the state machine understand the idempotent create
 * case because it's conceivable that an idempotent create request would
 * actually end up creating the bucket on a particular RGW. Perhaps there was
 * an error in a previous create, or we're doing some consolidation. In any
 * case, we need the create to succeed, and for the user to get a useful
 * return code.)
 *
 * 'User accessible' states are marked with asterisks in the diagrams below.
 * Other states are not user accessible, and an attempt to set them will
 * assert.
 *
 * This is the happy path:
 * ```
 * INIT -> *CREATE_START* ->
     ( CREATE_RPC_SUCCEEDED | CREATE_RPC_FAILED | CREATE_RPC_SOFT_FAILURE)
 *
 * CREATE_RPC_SUCCEEDED -> *UPDATE_START* ->
 *   ( UPDATE_RPC_SUCCEEDED | UPDATE_RPC_FAILED )
 *
 * UPDATE_RPC_SUCCEEDED -> *COMPLETE*
 *
 * (EXIT SUCCESS)
 * ```
 *
 * For the 'idempotent recreate' case, if we get AlreadyExists from gRPC,
 * meaning the same user already owns this bucket, then we go to
 * CREATE_RPC_SOFT_FAILURE, and update processing is skipped - we go directly
 * to COMPLETE.
 *
 * ```
 * INIT -> *CREATE_START* -> CREATE_RPC_SOFT_FAILURE
 *
 * CREATE_RPC_SOFT_FAILURE -> *UPDATE_START* -> COMPLETE
 *
 * (EXIT SUCCESS)
 * ```
 *
 * In the 'normal error' case (i.e. non-idempotent recreate), if the Create
 * RPC fails, perhaps because the bucket already exists, we go to
 * CREATE_RPC_FAILED and return a failure to the caller. This would typically
 * exit RGWCreateBucket::execute() with an error opcode.
 *
 * ```
 * ... -> CREATE_RPC_FAILED
 *
 * (EXIT FAILURE)
 * ```
 *
 * If the Update RPC fails, we enter state UPDATE_RPC_FAILED and return an
 * error to the user. This would typically exit RGWCreateBucket::execute().
 *
 * If something goes wrong in RGW during bucket creation and we exit
 * ::execute() in state CREATE_RPC_SUCCEEDED, we need to rollback the create
 * with a Delete operation. This is implemented in the destructor but can be
 * invoked manually if desired.
 *
 * ```
 * ... -> CREATE_RPC_SUCCEEDED -> *ROLLBACK_CREATE_START* ->
 *    ( ROLLBACK_CREATE_SUCCEEDED | ROLLBACK_CREATE_FAILED )
 *
 * (EXIT DESTRUCTOR (no status))
 * ```
 *
 * Other state transitions are not allowed, and an attempt will generate an
 * error in the logs. Again, an attempt to set a state not deemed 'user
 * accessible' will assert, because that's a programming error.
 *
 * @tparam T The UBNSClient implementation to use.
 */
template <typename T>
class UBNSCreateStateMachine {

public:
  enum class CreateMachineState {
    EMPTY,
    INIT,
    CREATE_START,
    CREATE_RPC_SUCCEEDED,
    CREATE_RPC_FAILED,
    CREATE_RPC_SOFT_FAILURE,
    UPDATE_START,
    UPDATE_RPC_SUCCEEDED,
    UPDATE_RPC_FAILED,
    ROLLBACK_CREATE_START,
    ROLLBACK_CREATE_SUCCEEDED,
    ROLLBACK_CREATE_FAILED,
    COMPLETE,
  }; // enum class UBNSCreateMachine::State

  std::string to_str(CreateMachineState state)
  {
    switch (state) {
    case CreateMachineState::EMPTY:
      return "EMPTY";
    case CreateMachineState::INIT:
      return "INIT";
    case CreateMachineState::CREATE_START:
      return "CREATE_START";
    case CreateMachineState::CREATE_RPC_SUCCEEDED:
      return "CREATE_RPC_SUCCEEDED";
    case CreateMachineState::CREATE_RPC_FAILED:
      return "CREATE_FAILED";
    case CreateMachineState::CREATE_RPC_SOFT_FAILURE:
      return "CREATE_RPC_SOFT_FAILURE";
    case CreateMachineState::UPDATE_START:
      return "UPDATE_START";
    case CreateMachineState::UPDATE_RPC_SUCCEEDED:
      return "UPDATE_RPC_SUCCEEDED";
    case CreateMachineState::UPDATE_RPC_FAILED:
      return "UPDATE_RPC_FAILED";
    case CreateMachineState::ROLLBACK_CREATE_START:
      return "ROLLBACK_CREATE_START";
    case CreateMachineState::ROLLBACK_CREATE_SUCCEEDED:
      return "ROLLBACK_CREATE_SUCCEEDED";
    case CreateMachineState::ROLLBACK_CREATE_FAILED:
      return "ROLLBACK_CREATE_FAILED";
    case CreateMachineState::COMPLETE:
      return "COMPLETE";
    }
  } // UBNSCreateMachine::to_str(State)

  /**
   * @brief Construct an 'empty' UBNSCreateStateMachine object.
   *
   * This is necessary so we can create a std::optional of these objects. The
   * compiler will object if there's not a default constructor.
   *
   * DO NOT attempt to turn an empty machine into a real machine by changing
   * its state - it'll assert!
   */
  UBNSCreateStateMachine()
      : dpp_ { nullptr }
      , client_ { nullptr }
      , state_ { CreateMachineState::EMPTY }
  {
  }

  /**
   * @brief Construct a new UBNSCreateStateMachine object.
   *
   * Construct a machine with all the parameters it needs to send proper gRPC
   * requests. The machine is created in the INIT state.
   *
   * @param dpp DoutPrefixProvider for logging.
   * @param client The UBNSClient object to use.
   * @param bucket_name The bucket name.
   * @param cluster_id The cluster ID, set at startup.
   * @param owner The bucket owner.
   */
  UBNSCreateStateMachine(const DoutPrefixProvider* dpp, std::shared_ptr<T> client, const std::string& bucket_name, const std::string cluster_id, const std::string& owner)
      : dpp_ { dpp }
      , client_ { client }
      , bucket_name_ { bucket_name }
      , cluster_id_ { cluster_id }
      , owner_ { owner }
      , state_ { CreateMachineState::INIT }
  {
    // Save some repetition when logging.
    bucket_log_id_ = fmt::format(FMT_STRING("bucket('{}','{}','{}')"), bucket_name_, cluster_id_, owner_);
  }

  /**
   * @brief Destroy the UBNSCreateStateMachine object, performing rollback if
   * required.
   *
   * This is not a passive destructor. If the machine is in a
   * partially-completed state, the destructor will send RPCs to try to keep
   * the upstream state machine in sync.
   */
  ~UBNSCreateStateMachine()
  {
    if (state_ == CreateMachineState::CREATE_RPC_SOFT_FAILURE) {
      // In the idempotent-repeated-create case, just mark the machine as
      // complete so there's no ambiguity in the logs.
      (void)set_state(CreateMachineState::COMPLETE);
    } else if (state_ == CreateMachineState::CREATE_RPC_SUCCEEDED) {
      ldpp_dout(dpp_, 1) << fmt::format(FMT_STRING("{}: Rolling back bucket creation for {}"), machine_id, bucket_log_id_) << dendl;
      // Start the rollback. Ignore the result.
      (void)set_state(CreateMachineState::ROLLBACK_CREATE_START);
    }
    ldpp_dout(dpp_, 1) << fmt::format(FMT_STRING("{}: destructor: {} end state {}"), machine_id, bucket_log_id_, to_str(state_)) << dendl;
  }

  // Delete copy and move constructors. We want to be very explicit about how
  // these machines are created. Leaving constructors around, especially move,
  // leaves opportunities for confusing log messages and bugs.
  UBNSCreateStateMachine(const UBNSCreateStateMachine&) = delete;
  UBNSCreateStateMachine(UBNSCreateStateMachine&&) = delete;
  UBNSCreateStateMachine& operator=(const UBNSCreateStateMachine&) = delete;
  UBNSCreateStateMachine& operator=(UBNSCreateStateMachine&&) = delete;

  /// Return the current state of the machine.
  CreateMachineState state() const noexcept { return state_; }

  /// Return a compact string representing the bucket, cluster and owner.
  std::string bucket_log_id() { return bucket_log_id_; }

  /// Return whether or not the state is valid for user transitions.
  bool is_user_state(CreateMachineState state) const noexcept
  {
    switch (state) {
    case CreateMachineState::CREATE_START:
    case CreateMachineState::UPDATE_START:
    case CreateMachineState::ROLLBACK_CREATE_START:
    case CreateMachineState::COMPLETE:
      return true;
    default:
      return false;
    }
  }

  /**
   * @brief Attempt to move the state machine.
   *
   * As documented in the class, there are only a few valid state transitions.
   * Invalid transitions will generate an error in the logs and return false.
   * Attempts to set a state not deemed user-accessible will assert.
   *
   * @param new_state The requested new state.
   * @return true on success.
   * @return false on failure.
   */
  bool set_state(CreateMachineState new_state) noexcept
  {
    ceph_assertf_always(state_ != CreateMachineState::EMPTY, "%s: attempt to set state on empty machine", machine_id);

    ldpp_dout(dpp_, 5) << fmt::format(FMT_STRING("{}: {}: attempt state transition {} -> {}"), machine_id, bucket_log_id_, to_str(state_), to_str(new_state)) << dendl;
    UBNSClientResult result;

    if (!is_user_state(new_state)) {
      ceph_assertf_always(false, "%s: %s: non-user state transition %s", machine_id, bucket_log_id_.c_str(), to_str(new_state).c_str());
    }

    // Implement our state machine. Return from here if the state was handled.
    // A 'break' here means an illegal state transition was attempted, and the
    // following code will log an error. We're implementing cases that will be
    // caught by !is_user_state() above in order to keep the compiler happy
    // (it will be annoyed if we're missing enum case instances) and because I
    // think it's clearer.
    //
    switch (new_state) {
    case CreateMachineState::EMPTY:
      break;

    case CreateMachineState::INIT:
      break;

    case CreateMachineState::CREATE_START:
      if (state_ != CreateMachineState::INIT) {
        break;
      }
      saved_result_.reset();
      result = client_->add_bucket_entry(dpp_, bucket_name_, cluster_id_, owner_);
      if (result.ok()) {
        state_ = CreateMachineState::CREATE_RPC_SUCCEEDED;
        ldpp_dout(dpp_, 5) << fmt::format(FMT_STRING("{}: add_bucket_entry() rpc for {} succeeded"), machine_id, bucket_log_id_) << dendl;
        return true;

      } else if (result.code() == ERR_UBNS_BUCKET_ALREADY_OWNED_BY_YOU) {
        // This is a special case. The bucket is already owned by the user, so
        // we can just move on to the next state.
        // Note: This won't match S3 - S3 apparently returns 409 here, whereas
        // if we let RGW continue to create a bucket that already exists, it
        // will return 200 OK.
        state_ = CreateMachineState::CREATE_RPC_SOFT_FAILURE;
        ldpp_dout(dpp_, 5) << fmt::format(FMT_STRING("{}: add_bucket_entry() rpc for {} returned {} '{}', setting state CREATE_RPC_SOFT_FAILURE"), machine_id, bucket_log_id_, result.code(), result.message()) << dendl;
        return true;

      } else {
        ldpp_dout(dpp_, 1) << fmt::format(FMT_STRING("{}: add_bucket_entry() rpc for {} failed: {}"), machine_id, bucket_log_id_, result.to_string()) << dendl;
        state_ = CreateMachineState::CREATE_RPC_FAILED;
        saved_result_ = result;
        return false;
      }
      break;

    case CreateMachineState::CREATE_RPC_SUCCEEDED:
      break;

    case CreateMachineState::CREATE_RPC_FAILED:
      break;

    case CreateMachineState::CREATE_RPC_SOFT_FAILURE:
      break;

    case CreateMachineState::UPDATE_START:
      if (state_ == CreateMachineState::CREATE_RPC_SOFT_FAILURE) {
        // The Create RPC failed with the 'bucket already owned by user'
        // error, so we don't need the update - the bucket is already here.
        ldpp_dout(dpp_, 5) << fmt::format(FMT_STRING("{}: skipping update_bucket_entry() rpc for {} in CREATE_RPC_SOFT_FAILURE state"), machine_id, bucket_log_id_) << dendl;
        state_ = CreateMachineState::COMPLETE;
        return true;
      }
      if (state_ != CreateMachineState::CREATE_RPC_SUCCEEDED && state_ != CreateMachineState::UPDATE_RPC_FAILED) {
        break;
      }
      saved_result_.reset();
      result = client_->update_bucket_entry(dpp_, bucket_name_, cluster_id_, owner_, UBNSBucketUpdateState::CREATED);
      if (result.ok()) {
        state_ = CreateMachineState::UPDATE_RPC_SUCCEEDED;
        ldpp_dout(dpp_, 5) << fmt::format(FMT_STRING("{}: update_bucket_entry() rpc for {} succeeded"), machine_id, bucket_log_id_) << dendl;
        return true;
      } else {
        ldpp_dout(dpp_, 1) << fmt::format(FMT_STRING("{}: update_bucket_entry() rpc for {} failed: {}"), machine_id, bucket_log_id_, result.to_string()) << dendl;
        state_ = CreateMachineState::UPDATE_RPC_FAILED;
        saved_result_ = result;
        return false;
      }
      break;

    case CreateMachineState::UPDATE_RPC_SUCCEEDED:
      break;

    case CreateMachineState::UPDATE_RPC_FAILED:
      break;

    case CreateMachineState::ROLLBACK_CREATE_START:
      if (state_ != CreateMachineState::CREATE_RPC_SUCCEEDED && state_ != CreateMachineState::UPDATE_RPC_FAILED) {
        break;
      }
      saved_result_.reset();
      result = client_->delete_bucket_entry(dpp_, bucket_name_, cluster_id_, owner_);
      if (result.ok()) {
        state_ = CreateMachineState::ROLLBACK_CREATE_SUCCEEDED;
        ldpp_dout(dpp_, 5) << fmt::format(FMT_STRING("{}: rollback delete_bucket_entry() rpc for {} succeeded"), machine_id, bucket_log_id_) << dendl;
        return true;
      } else {
        state_ = CreateMachineState::ROLLBACK_CREATE_FAILED;
        saved_result_ = result;
        return false;
      }
      break;

    case CreateMachineState::ROLLBACK_CREATE_SUCCEEDED:
      break;

    case CreateMachineState::ROLLBACK_CREATE_FAILED:
      break;

    case CreateMachineState::COMPLETE:
      if (state_ != CreateMachineState::UPDATE_RPC_SUCCEEDED && state_ != CreateMachineState::CREATE_RPC_SOFT_FAILURE) {
        break;
      }
      state_ = CreateMachineState::COMPLETE;
      return true;
    }
    // If we didn't return directly from the state switch, we're attempting an invalid
    // transition.
    ldpp_dout(dpp_, 1) << fmt::format(FMT_STRING("{}: {}: invalid state transition {} -> {}"), machine_id, bucket_log_id_, to_str(state_), to_str(new_state)) << dendl;
    return false;
  }

  /**
   * @brief Return the last gRPC failure result, if any.
   *
   * The result is unpacked into an object that's safe to return to non-gRPC
   * code world.
   *
   * @return std::optional<UBNSClientResult> an optional UBNSClientResult.
   */
  std::optional<UBNSClientResult> saved_grpc_result() const noexcept { return saved_result_; }

private:
  const DoutPrefixProvider* dpp_;
  std::shared_ptr<T> client_;
  std::string bucket_name_;
  std::string cluster_id_;
  std::string owner_;
  std::string bucket_log_id_;
  CreateMachineState state_;
  std::optional<UBNSClientResult> saved_result_;
  constexpr static const char* machine_id = "UBNSCreate";
}; // class UBNSCreateMachine

/// Convenience typedef for rgw_op.cc.
using UBNSCreateMachine = UBNSCreateStateMachine<UBNSClient>;
/// Convenience typedef for rgw_op.cc.
using UBNSCreateState = UBNSCreateMachine::CreateMachineState;

/**
 * @brief State machine implementing the client Delete side of the UBNS
 * protocol.
 *
 * This state machine is used to implement the two-phase commit protocol for
 * UBNS bucket deletion. The formality is so that we can create a RAII object
 * that does the proper rollback regardless of the method by which
 * RGWDeleteBucket::execute() is exited.
 *
 * (The majority of exit points from RGWDeleteBucket::execute() are error
 * exits; manually cleaning up the UBNS state in each would be messy,
 * error-prone and hard-to-maintain.)
 *
 * The state machine is implemented as a template so we can instantiate a
 * non-gRPC version in the unit tests and test the machine completely
 * standalone.
 *
 * The machine is simple. Given three gRPC services, one for the initial
 * Bucket Update request to set the bucket in the 'deleting' state, one Delete
 * request to actually mark the bucket as deleted once the RGW deletion has
 * succeeded, and one Update request to rollback the 'deleting' state if the
 * RGW delete fails.
 *
 * 'User accessible' states are marked with asterisks in the diagrams below.
 * Other states are not user accessible, and an attempt to set them will
 * assert.
 *
 * This is the happy path:
 * ```
 * INIT -> *UPDATE_START* -> ( UPDATE_RPC_SUCCEEDED | UPDATE_RPC_FAILED )
 *
 * UPDATE_RPC_SUCCEEDED -> *DELETE_START* -> ( DELETE_RPC_SUCCEEDED | DELETE_RPC_FAILED )
 *
 * DELETE_RPC_SUCCEEDED -> *COMPLETE*
 *
 * (EXIT SUCCESS)
 * ```
 *
 * If the Update RPC fails, perhaps because the bucket is not in the 'created'
 * state, we go to UPDATE_RPC_FAILED and return a failure to the caller. This
 * would typically exit RGWDeleteBucket::execute() with an error opcode.
 *
 * ```
 * ... -> UPDATE_RPC_FAILED
 *
 * (EXIT FAILURE)
 * ```
 *
 * If the Delete RPC fails, we enter state DELETE_RPC_FAILED and return an
 * error to the user. This would typically exit RGWDeleteBucket::execute().
 *
 * If something goes wrong in RGW during bucket deletion and we exit
 * ::execute() in state DELETE_RPC_SUCCEEDED, we need to rollback the update
 * so the bucket is back in its 'created' state. This is implemented in the
 * destructor but can be invoked manually if desired.
 *
 * ```
 * ... -> DELETE_RPC_SUCCEEDED -> *ROLLBACK_UPDATE_START* ->
 *    ( ROLLBACK_UPDATE_SUCCEEDED | ROLLBACK_UPDATE_FAILED )
 *
 * (EXIT DESTRUCTOR (no status))
 * ```
 *
 * Other state transitions are not allowed, and an attempt will generate an
 * error in the logs. Again, an attempt to set a state not deemed 'user
 * accessible' will assert, because that's a programming error.
 *
 * @tparam T The UBNSClient implementation to use.

 */
template <typename T>
class UBNSDeleteStateMachine {

public:
  enum class DeleteMachineState {
    EMPTY,
    INIT,
    UPDATE_START,
    UPDATE_RPC_SUCCEEDED,
    UPDATE_RPC_FAILED,
    DELETE_START,
    DELETE_RPC_SUCCEEDED,
    DELETE_RPC_FAILED,
    ROLLBACK_UPDATE_START,
    ROLLBACK_UPDATE_SUCCEEDED,
    ROLLBACK_UPDATE_FAILED,
    COMPLETE
  }; // enum class UBNSDeleteMachine::State

  std::string to_str(DeleteMachineState state)
  {
    switch (state) {
    case DeleteMachineState::EMPTY:
      return "EMPTY";
    case DeleteMachineState::INIT:
      return "INIT";
    case DeleteMachineState::UPDATE_START:
      return "UPDATE_START";
    case DeleteMachineState::UPDATE_RPC_SUCCEEDED:
      return "UPDATE_RPC_SUCCEEDED";
    case DeleteMachineState::UPDATE_RPC_FAILED:
      return "UPDATE_RPC_FAILED";
    case DeleteMachineState::DELETE_START:
      return "DELETE_START";
    case DeleteMachineState::DELETE_RPC_SUCCEEDED:
      return "DELETE_RPC_SUCCEEDED";
    case DeleteMachineState::DELETE_RPC_FAILED:
      return "DELETE_RPC_FAILED";
    case DeleteMachineState::ROLLBACK_UPDATE_START:
      return "ROLLBACK_UPDATE_START";
    case DeleteMachineState::ROLLBACK_UPDATE_SUCCEEDED:
      return "ROLLBACK_UPDATE_SUCCEEDED";
    case DeleteMachineState::ROLLBACK_UPDATE_FAILED:
      return "ROLLBACK_UPDATE_FAILED";
    case DeleteMachineState::COMPLETE:
      return "COMPLETE";
    }
  }; // UBNSDeleteMachine::to_str(State)

  /**
   * @brief Construct an 'empty' UBNSDeleteStateMachine object.
   *
   * This is necessary so we can create a std::optional of these objects. The
   * compiler will object if there's not a default constructor.
   *
   * DO NOT attempt to turn an empty machine into a real machine by changing
   * its state - it'll assert!
   */
  UBNSDeleteStateMachine()
      : dpp_ { nullptr }
      , client_ { nullptr }
      , state_ { DeleteMachineState::EMPTY }
  {
  }

  /**
   * @brief Construct a new UBNSDeleteStateMachine object.
   *
   * Construct a machine with all the parameters it needs to send proper gRPC
   * requests. The machine is created in the INIT state.
   *
   * @param dpp DoutPrefixProvider for logging.
   * @param client The UBNSClient object to use.
   * @param bucket_name The bucket name.
   * @param cluster_id The cluster ID, set at startup.
   * @param owner The bucket owner (not really used for delete, but set here anyway).
   */
  UBNSDeleteStateMachine(const DoutPrefixProvider* dpp, std::shared_ptr<T> client, const std::string& bucket_name, const std::string& cluster_id, const std::string& owner)
      : dpp_ { dpp }
      , client_ { client }
      , bucket_name_ { bucket_name }
      , cluster_id_ { cluster_id }
      , owner_ { owner }
      , state_ { DeleteMachineState::INIT }
  {
    // Save some repetition when logging.
    bucket_log_id_ = fmt::format(FMT_STRING("bucket('{}','{}','{}')"), bucket_name_, cluster_id_, owner_);
  }

  /**
   * @brief Destroy the UBNSDeleteStateMachine object, performing rollback if
   * required.
   *
   * This is not a passive destructor. If the machine is in a
   * partially-completed state, the destructor will send RPCs to try to keep
   * the upstream state machine in sync.
   */
  ~UBNSDeleteStateMachine()
  {
    if (state_ == DeleteMachineState::UPDATE_RPC_SUCCEEDED) {
      ldpp_dout(dpp_, 1) << fmt::format(FMT_STRING("{}: rolling back bucket deletion update for {}"), machine_id, bucket_log_id_) << dendl;
      // Start the rollback. Ignore the result.
      (void)set_state(DeleteMachineState::ROLLBACK_UPDATE_START);
    }
    ldpp_dout(dpp_, 1) << fmt::format(FMT_STRING("{}: destructor: {} end state {}"), machine_id, bucket_log_id_, to_str(state_)) << dendl;
  }

  // Delete copy and move constructors. We want to be very explicit about how
  // these machines are created. Leaving constructors around, especially move,
  // leaves opportunities for confusing log messages and bugs.
  UBNSDeleteStateMachine(const UBNSDeleteStateMachine&) = delete;
  UBNSDeleteStateMachine& operator=(const UBNSDeleteStateMachine&) = delete;
  UBNSDeleteStateMachine(UBNSDeleteStateMachine&&) = delete;
  UBNSDeleteStateMachine& operator=(UBNSDeleteStateMachine&&) = delete;

  /// Return the current state of the machine.
  DeleteMachineState state() const noexcept { return state_; }

  /// Return a compact string representing the bucket, cluster and owner.
  std::string bucket_log_id() { return bucket_log_id_; }

  /// Return whether or not the state is valid for user transitions.
  bool is_user_state(DeleteMachineState state) const noexcept
  {
    switch (state) {
    case DeleteMachineState::UPDATE_START:
    case DeleteMachineState::DELETE_START:
    case DeleteMachineState::ROLLBACK_UPDATE_START:
    case DeleteMachineState::COMPLETE:
      return true;
    default:
      return false;
    }
  }

  /**
   * @brief Attempt to move the state machine.
   *
   * As documented in the class, there are only a few valid state transitions.
   * Invalid transitions will generate an error in the logs and return false.
   * Attempts to set a state not deemed user-accessible will assert.
   *
   * @param new_state The requested new state.
   * @return true on success.
   * @return false on failure.
   */
  bool set_state(DeleteMachineState new_state) noexcept
  {
    ceph_assertf_always(state_ != DeleteMachineState::EMPTY, "%s: attempt to set state on empty machine", machine_id);

    ldpp_dout(dpp_, 5) << fmt::format(FMT_STRING("{}: {}: attempt state transition {} -> {}"), machine_id, bucket_log_id_, to_str(state_), to_str(new_state)) << dendl;
    UBNSClientResult result;

    if (!is_user_state(new_state)) {
      ceph_assertf_always(false, "%s: %s: non-user state transition %s attempted", machine_id, bucket_log_id_.c_str(), to_str(new_state).c_str());
    }

    // Implement our state machine. Return from here if the state was handler.
    // A 'break' means an illegal state transition. We're implementing cases
    // that will be caught by !is_user_state() above in order to keep the
    // compiler happy (it will be annoyed if we're missing enum case
    // instances) and because I think it's clearer.
    //
    switch (new_state) {
    case DeleteMachineState::EMPTY:
      break;

    case DeleteMachineState::INIT:
      break;

    case DeleteMachineState::UPDATE_START:
      if (state_ != DeleteMachineState::INIT) {
        break;
      }
      result = client_->update_bucket_entry(dpp_, bucket_name_, cluster_id_, owner_, UBNSBucketUpdateState::DELETING);
      if (result.ok()) {
        state_ = DeleteMachineState::UPDATE_RPC_SUCCEEDED;
        ldpp_dout(dpp_, 5) << fmt::format(FMT_STRING("{}: update_bucket_entry() rpc for {} succeeded"), machine_id, bucket_log_id_) << dendl;
        return true;
      } else {
        ldpp_dout(dpp_, 1) << fmt::format(FMT_STRING("{}: update_bucket_entry() rpc for {} failed: {}"), machine_id, bucket_log_id_, result.to_string()) << dendl;
        state_ = DeleteMachineState::UPDATE_RPC_FAILED;
        saved_result_ = result;
        return false;
      }
      break;

    case DeleteMachineState::UPDATE_RPC_SUCCEEDED:
      break;

    case DeleteMachineState::UPDATE_RPC_FAILED:
      break;

    case DeleteMachineState::DELETE_START:
      if (state_ != DeleteMachineState::UPDATE_RPC_SUCCEEDED && state_ != DeleteMachineState::DELETE_RPC_FAILED) {
        break;
      }
      result = client_->delete_bucket_entry(dpp_, bucket_name_, cluster_id_, owner_);
      if (result.ok()) {
        state_ = DeleteMachineState::DELETE_RPC_SUCCEEDED;
        ldpp_dout(dpp_, 5) << fmt::format(FMT_STRING("{}: delete_bucket_entry() rpc for {} succeeded"), machine_id, bucket_log_id_) << dendl;
        return true;
      } else {
        ldpp_dout(dpp_, 1)
            << fmt::format(FMT_STRING("{}: delete_bucket_entry() rpc for {} failed: {}"), machine_id, bucket_log_id_, result.to_string())
            << dendl;
        state_ = DeleteMachineState::DELETE_RPC_FAILED;
        saved_result_ = result;
        return false;
      }
      break;

    case DeleteMachineState::DELETE_RPC_SUCCEEDED:
      break;

    case DeleteMachineState::DELETE_RPC_FAILED:
      break;

    case DeleteMachineState::ROLLBACK_UPDATE_START:
      if (state_ != DeleteMachineState::UPDATE_RPC_SUCCEEDED && state_ != DeleteMachineState::DELETE_RPC_FAILED) {
        break;
      }
      ldpp_dout(dpp_, 1) << fmt::format(FMT_STRING("{}: rolling back bucket deletion update for {}"), machine_id, bucket_log_id_)
                         << dendl;
      result = client_->update_bucket_entry(dpp_, bucket_name_, cluster_id_, owner_, UBNSBucketUpdateState::CREATED);
      if (result.ok()) {
        state_ = DeleteMachineState::ROLLBACK_UPDATE_SUCCEEDED;
        ldpp_dout(dpp_, 5) << fmt::format(FMT_STRING("{}: rollback update_bucket_entry() rpc for {} succeeded"), machine_id, bucket_log_id_) << dendl;
        return true;
      } else {
        state_ = DeleteMachineState::ROLLBACK_UPDATE_FAILED;
        saved_result_ = result;
        return false;
      }
      break;

    case DeleteMachineState::ROLLBACK_UPDATE_SUCCEEDED:
      break;

    case DeleteMachineState::ROLLBACK_UPDATE_FAILED:
      break;

    case DeleteMachineState::COMPLETE:
      if (state_ != DeleteMachineState::DELETE_RPC_SUCCEEDED) {
        break;
      }
      state_ = DeleteMachineState::COMPLETE;
      return true;
    }

    // If we didn't return from the state switch, we're attempting an invalid
    // transition.
    ldpp_dout(dpp_, 1) << fmt::format(FMT_STRING("{}: {}: invalid state transition {} -> {}"), machine_id, bucket_log_id_, to_str(state_), to_str(new_state)) << dendl;
    return false;
  }

  /**
   * @brief Return the last gRPC failure result, if any.
   *
   * The result is unpacked into an object that's safe to return to non-gRPC
   * code world.
   *
   * @return std::optional<UBNSClientResult> an optional UBNSClientResult.
   */
  std::optional<UBNSClientResult> saved_grpc_result() const noexcept { return saved_result_; }

private:
  const DoutPrefixProvider* dpp_;
  std::shared_ptr<T> client_;
  std::string bucket_name_;
  std::string cluster_id_;
  std::string owner_;
  std::string bucket_log_id_;
  DeleteMachineState state_;
  std::optional<UBNSClientResult> saved_result_;
  constexpr static const char* machine_id = "UBNSDelete";
}; // class UBNSDeleteMachine

/// Convenience typedef for rgw_op.cc.
using UBNSDeleteMachine = UBNSDeleteStateMachine<UBNSClient>;
/// Convenience typedef for rgw_op.cc.
using UBNSDeleteState = UBNSDeleteMachine::DeleteMachineState;

} // namespace rgw
