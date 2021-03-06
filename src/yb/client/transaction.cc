//
// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//
//

#include "yb/client/transaction.h"

#include <unordered_set>

#include "yb/client/async_rpc.h"
#include "yb/client/client.h"
#include "yb/client/in_flight_op.h"
#include "yb/client/meta_cache.h"
#include "yb/client/tablet_rpc.h"
#include "yb/client/transaction_manager.h"
#include "yb/client/transaction_rpc.h"
#include "yb/client/yb_op.h"

#include "yb/common/transaction.h"

#include "yb/rpc/messenger.h"
#include "yb/rpc/rpc.h"
#include "yb/rpc/scheduler.h"

#include "yb/util/logging.h"
#include "yb/util/random_util.h"
#include "yb/util/result.h"
#include "yb/util/strongly_typed_bool.h"

using namespace std::placeholders;

DEFINE_uint64(transaction_heartbeat_usec, 500000, "Interval of transaction heartbeat in usec.");
DEFINE_bool(transaction_disable_heartbeat_in_tests, false, "Disable heartbeat during test.");
DECLARE_uint64(max_clock_skew_usec);

namespace yb {
namespace client {

namespace {

TransactionMetadata CreateMetadata(IsolationLevel isolation, HybridTime read_time) {
  return {GenerateTransactionId(), isolation, TabletId(), RandomUniformInt<uint64_t>(), read_time};
}

YB_STRONGLY_TYPED_BOOL(Child);
YB_DEFINE_ENUM(TransactionState, (kRunning)(kAborted)(kCommitted));

} // namespace

Result<ChildTransactionData> ChildTransactionData::FromPB(const ChildTransactionDataPB& data) {
  ChildTransactionData result;
  auto metadata = TransactionMetadata::FromPB(data.metadata());
  RETURN_NOT_OK(metadata);
  result.metadata = std::move(*metadata);
  result.read_time = ReadHybridTime::FromReadTimePB(data);
  for (const auto& entry : data.local_limits()) {
    result.local_limits.emplace(entry.first, HybridTime(entry.second));
  }
  return result;
}

class YBTransaction::Impl final {
 public:
  Impl(TransactionManager* manager, YBTransaction* transaction, IsolationLevel isolation)
      : manager_(manager),
        transaction_(transaction),
        read_point_(manager->clock()),
        child_(Child::kFalse) {
    if (isolation == IsolationLevel::SNAPSHOT_ISOLATION) {
      read_point_.SetCurrentReadTime();
      metadata_ = CreateMetadata(isolation, read_point_.GetReadTime());
    } else {
      // TODO: The choice of read time should be reviewed when implementing serializable
      // transactions.
      metadata_ = CreateMetadata(isolation, manager->Now());
    }
    Init();
    VLOG_WITH_PREFIX(1) << "Started, metadata: " << metadata_;
  }

  Impl(TransactionManager* manager, YBTransaction* transaction, ChildTransactionData data)
      : manager_(manager),
        transaction_(transaction),
        read_point_(manager->clock()),
        child_(Child::kTrue) {
    read_point_.SetReadTime(std::move(data.read_time), std::move(data.local_limits));
    metadata_ = std::move(data.metadata);
    Init();
    VLOG_WITH_PREFIX(1) << "Started child, metadata: " << metadata_;
    ready_ = true;
  }

  ~Impl() {
    manager_->rpcs().Abort({&heartbeat_handle_, &commit_handle_, &abort_handle_});
  }

  YBTransactionPtr CreateSimilarTransaction() {
    return std::make_shared<YBTransaction>(manager_, metadata_.isolation);
  }

  // This transaction is a restarted transaction, so we set it up with data from original one.
  void SetupRestart(Impl* other) {
    VLOG_WITH_PREFIX(1) << "Setup from " << other->ToString();
    auto transaction = transaction_->shared_from_this();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (state_.load(std::memory_order_acquire) != TransactionState::kRunning) {
        LOG(DFATAL) << "Restart of completed transaction";
        return;
      }
      DCHECK(read_point_.IsRestartRequired());
      other->read_point_ = std::move(read_point_);
      other->read_point_.Restart();
      state_.store(TransactionState::kAborted, std::memory_order_release);
    }
    DoAbort(Status::OK(), transaction);
  }

  bool Prepare(const std::unordered_set<internal::InFlightOpPtr>& ops,
               Waiter waiter,
               TransactionMetadata* metadata) {
    CHECK_NOTNULL(metadata);

    VLOG_WITH_PREFIX(1) << "Prepare";

    bool has_tablets_without_parameters = false;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      if (!ready_) {
        waiters_.push_back(std::move(waiter));
        lock.unlock();
        RequestStatusTablet();
        VLOG_WITH_PREFIX(1) << "Prepare, rejected";
        return false;
      }

      for (const auto& op : ops) {
        VLOG_WITH_PREFIX(1) << "Prepare, op: " << op->ToString();
        DCHECK(op->tablet != nullptr);
        auto it = tablets_.find(op->tablet->tablet_id());
        if (it == tablets_.end()) {
          tablets_.emplace(op->tablet->tablet_id(), TabletState());
          has_tablets_without_parameters = true;
        } else if (!has_tablets_without_parameters) {
          has_tablets_without_parameters = !it->second.has_parameters;
        }
      }
    }

    if (has_tablets_without_parameters) {
      *metadata = metadata_;
    } else {
      metadata->transaction_id = metadata_.transaction_id;
    }
    return true;
  }

  void Flushed(const internal::InFlightOps& ops, const Status& status) {
    if (status.ok()) {
      std::lock_guard<std::mutex> lock(mutex_);
      TabletStates::iterator it = tablets_.end();
      for (const auto& op : ops) {
        if (op->yb_op->succeeded()) {
          const std::string& tablet_id = op->tablet->tablet_id();
          // Usually all ops belong to the same tablet. So we can avoid repeating lookup.
          if (it == tablets_.end() || it->first != tablet_id) {
            auto it = tablets_.find(tablet_id);
            CHECK(it != tablets_.end());
            it->second.has_parameters = true;
          }
        }
      }
    } else if (status.IsTryAgain()) {
      SetError(status);
    }
    // We should not handle other errors, because it is just notification that batch was failed.
    // And they are handled during processing of that batch.
  }

  void Commit(CommitCallback callback) {
    auto transaction = transaction_->shared_from_this();
    {
      std::unique_lock<std::mutex> lock(mutex_);
      auto status = CheckRunning(&lock);
      if (!status.ok()) {
        callback(status);
        return;
      }
      if (child_) {
        callback(STATUS(IllegalState, "Commit of child transaction is not allowed"));
        return;
      }
      if (IsRestartRequired()) {
        callback(STATUS(
            IllegalState, "Commit of transaction that requires restart is not allowed"));
        return;
      }
      state_.store(TransactionState::kCommitted, std::memory_order_release);
      commit_callback_ = std::move(callback);
      if (!ready_) {
        waiters_.emplace_back(std::bind(&Impl::DoCommit, this, _1, transaction));
        lock.unlock();
        RequestStatusTablet();
        return;
      }
    }
    DoCommit(Status::OK(), transaction);
  }

  void Abort() {
    auto transaction = transaction_->shared_from_this();
    {
      std::unique_lock<std::mutex> lock(mutex_);
      auto state = state_.load(std::memory_order_acquire);
      if (state != TransactionState::kRunning) {
        LOG_IF(DFATAL, state != TransactionState::kAborted) << "Abort of committed transaction";
        return;
      }
      if (child_) {
        LOG(DFATAL) << "Abort of child transaction";
        return;
      }
      state_.store(TransactionState::kAborted, std::memory_order_release);
      if (!ready_) {
        waiters_.emplace_back(std::bind(&Impl::DoAbort, this, _1, transaction));
        lock.unlock();
        RequestStatusTablet();
        return;
      }
    }
    DoAbort(Status::OK(), transaction);
  }

  bool IsRestartRequired() const {
    return read_point_.IsRestartRequired();
  }

  std::shared_future<TransactionMetadata> TEST_GetMetadata() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (metadata_future_.valid()) {
      return metadata_future_;
    }
    metadata_future_ = std::shared_future<TransactionMetadata>(metadata_promise_.get_future());
    if (!ready_) {
      auto transaction = transaction_->shared_from_this();
      waiters_.push_back([this, transaction](const Status& status) {
        // OK to crash here, because we are in test
        CHECK_OK(status);
        metadata_promise_.set_value(metadata_);
      });
      lock.unlock();
      RequestStatusTablet();
    }
    metadata_promise_.set_value(metadata_);
    return metadata_future_;
  }

  void PrepareChild(PrepareChildCallback callback) {
    auto transaction = transaction_->shared_from_this();
    std::unique_lock<std::mutex> lock(mutex_);
    auto status = CheckRunning(&lock);
    if (!status.ok()) {
      callback(status);
      return;
    }
    if (IsRestartRequired()) {
      lock.unlock();
      callback(STATUS(IllegalState, "Restart required"));
      return;
    }
    if (!ready_) {
      waiters_.emplace_back(std::bind(
          &Impl::DoPrepareChild, this, _1, transaction, std::move(callback), nullptr /* lock */));
      lock.unlock();
      RequestStatusTablet();
      return;
    }

    DoPrepareChild(Status::OK(), transaction, std::move(callback), &lock);
  }

  Result<ChildTransactionResultPB> FinishChild() {
    std::unique_lock<std::mutex> lock(mutex_);
    RETURN_NOT_OK(CheckRunning(&lock));
    if (!child_) {
      return STATUS(IllegalState, "Finish child of non child transaction");
    }
    state_.store(TransactionState::kCommitted, std::memory_order_release);
    ChildTransactionResultPB result;
    auto& tablets = *result.mutable_tablets();
    tablets.Reserve(tablets_.size());
    for (const auto& tablet : tablets_) {
      auto& out = *tablets.Add();
      out.set_tablet_id(tablet.first);
      tablet.second.ToPB(&out);
    }
    read_point_.FinishChildTransactionResult(&result);
    return result;
  }

  Status ApplyChildResult(const ChildTransactionResultPB& result) {
    std::unique_lock<std::mutex> lock(mutex_);
    RETURN_NOT_OK(CheckRunning(&lock));
    if (child_) {
      return STATUS(IllegalState, "Apply child result of child transaction");
    }

    for (const auto& tablet : result.tablets()) {
      tablets_[tablet.tablet_id()].MergeFromPB(tablet);
    }
    read_point_.ApplyChildTransactionResult(result);

    return Status::OK();
  }

  const std::string& LogPrefix() {
    return log_prefix_;
  }

  std::string ToString() const {
    return Format("Transaction: $0", metadata_.transaction_id);
  }

  const TransactionId& id() const {
    return metadata_.transaction_id;
  }

  ConsistentReadPoint& read_point() {
    return read_point_;
  }

 private:
  void Init() {
    log_prefix_ = Format("$0: ", to_string(metadata_.transaction_id));
    heartbeat_handle_ = manager_->rpcs().InvalidHandle();
    commit_handle_ = manager_->rpcs().InvalidHandle();
    abort_handle_ = manager_->rpcs().InvalidHandle();
  }

  CHECKED_STATUS CheckRunning(std::unique_lock<std::mutex>* lock) {
    if (state_.load(std::memory_order_acquire) != TransactionState::kRunning) {
      auto status = error_;
      lock->unlock();
      if (status.ok()) {
        status = STATUS(IllegalState, "Transaction already completed");
      }
      return status;
    }
    return Status::OK();
  }

  void DoCommit(const Status& status, const YBTransactionPtr& transaction) {
    VLOG_WITH_PREFIX(1) << Format("Commit, tablets: $0, status: $1", tablets_, status);

    if (!status.ok()) {
      commit_callback_(status);
      return;
    }

    // tablets_.empty() means that transaction does not have writes, so just abort it.
    // But notify caller that commit was successful, so it is transparent for him.
    if (tablets_.empty()) {
      DoAbort(Status::OK(), transaction);
      commit_callback_(Status::OK());
      return;
    }

    tserver::UpdateTransactionRequestPB req;
    req.set_tablet_id(status_tablet_->tablet_id());
    req.set_propagated_hybrid_time(manager_->Now().ToUint64());
    auto& state = *req.mutable_state();
    state.set_transaction_id(metadata_.transaction_id.begin(), metadata_.transaction_id.size());
    state.set_status(TransactionStatus::COMMITTED);
    for (const auto& tablet : tablets_) {
      state.add_tablets(tablet.first);
    }

    manager_->rpcs().RegisterAndStart(
        UpdateTransaction(
            TransactionRpcDeadline(),
            status_tablet_.get(),
            manager_->client().get(),
            &req,
            std::bind(&Impl::CommitDone, this, _1, _2, transaction)),
        &commit_handle_);
  }

  void DoAbort(const Status& status, const YBTransactionPtr& transaction) {
    VLOG_WITH_PREFIX(1) << Format("Abort, status: $1", status);

    if (!status.ok()) {
      // We already stopped to send heartbeats, so transaction would be aborted anyway.
      LOG(WARNING) << "Failed to abort transaction: " << status;
      return;
    }

    tserver::AbortTransactionRequestPB req;
    req.set_tablet_id(status_tablet_->tablet_id());
    req.set_propagated_hybrid_time(manager_->Now().ToUint64());
    req.set_transaction_id(metadata_.transaction_id.begin(), metadata_.transaction_id.size());

    manager_->rpcs().RegisterAndStart(
        AbortTransaction(
            TransactionRpcDeadline(),
            status_tablet_.get(),
            manager_->client().get(),
            &req,
            std::bind(&Impl::AbortDone, this, _1, _2, transaction)),
        &abort_handle_);
  }

  void CommitDone(const Status& status,
                  HybridTime propagated_hybrid_time,
                  const YBTransactionPtr& transaction) {
    VLOG_WITH_PREFIX(1) << "Committed: " << status;

    manager_->UpdateClock(propagated_hybrid_time);
    manager_->rpcs().Unregister(&commit_handle_);
    commit_callback_(status);
  }

  void AbortDone(const Status& status,
                 const tserver::AbortTransactionResponsePB& response,
                 const YBTransactionPtr& transaction) {
    VLOG_WITH_PREFIX(1) << "Aborted: " << status;

    if (response.has_propagated_hybrid_time()) {
      manager_->UpdateClock(HybridTime(response.propagated_hybrid_time()));
    }
    manager_->rpcs().Unregister(&abort_handle_);
  }

  void RequestStatusTablet() {
    bool expected = false;
    if (!requested_status_tablet_.compare_exchange_strong(
        expected, true, std::memory_order_acq_rel)) {
      return;
    }
    manager_->PickStatusTablet(
        std::bind(&Impl::StatusTabletPicked, this, _1, transaction_->shared_from_this()));
  }

  void StatusTabletPicked(const Result<std::string>& tablet,
                          const YBTransactionPtr& transaction) {
    VLOG_WITH_PREFIX(1) << "Picked status tablet: " << tablet;

    if (tablet.ok()) {
      manager_->client()->LookupTabletById(
          *tablet,
          TransactionRpcDeadline(),
          &status_tablet_holder_,
          Bind(&Impl::LookupTabletDone, Unretained(this), transaction), true /* use fast path */);
    } else {
      SetError(tablet.status());
    }
  }

  void LookupTabletDone(const YBTransactionPtr& transaction, const Status& status) {
    VLOG_WITH_PREFIX(1) << "Lookup tablet done: " << status;

    {
      std::lock_guard<std::mutex> lock(mutex_);
      status_tablet_ = std::move(status_tablet_holder_);
      metadata_.status_tablet = status_tablet_->tablet_id();
    }
    SendHeartbeat(TransactionStatus::CREATED, transaction_->shared_from_this());
  }

  void SendHeartbeat(TransactionStatus status,
                     std::weak_ptr<YBTransaction> weak_transaction) {
    auto transaction = weak_transaction.lock();
    if (!transaction || state_.load(std::memory_order_acquire) != TransactionState::kRunning) {
      return;
    }

    if (status != TransactionStatus::CREATED &&
        GetAtomicFlag(&FLAGS_transaction_disable_heartbeat_in_tests)) {
      HeartbeatDone(Status::OK(), HybridTime::kInvalid, status, transaction);
      return;
    }

    tserver::UpdateTransactionRequestPB req;
    req.set_tablet_id(status_tablet_->tablet_id());
    req.set_propagated_hybrid_time(manager_->Now().ToUint64());
    auto& state = *req.mutable_state();
    state.set_transaction_id(metadata_.transaction_id.begin(), metadata_.transaction_id.size());
    state.set_status(status);
    manager_->rpcs().RegisterAndStart(
        UpdateTransaction(
            TransactionRpcDeadline(),
            status_tablet_.get(),
            manager_->client().get(),
            &req,
            std::bind(&Impl::HeartbeatDone, this, _1, _2, status, transaction)),
        &heartbeat_handle_);
  }

  void HeartbeatDone(const Status& status,
                     HybridTime propagated_hybrid_time,
                     TransactionStatus transaction_status,
                     const YBTransactionPtr& transaction) {
    manager_->UpdateClock(propagated_hybrid_time);
    manager_->rpcs().Unregister(&heartbeat_handle_);

    if (status.ok()) {
      if (transaction_status == TransactionStatus::CREATED) {
        std::vector<Waiter> waiters;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          DCHECK(!ready_);
          ready_ = true;
          waiters_.swap(waiters);
        }
        VLOG_WITH_PREFIX(1) << "Created, notifying waiters: " << waiters.size();
        for (const auto& waiter : waiters) {
          waiter(Status::OK());
        }
      }
      std::weak_ptr<YBTransaction> weak_transaction(transaction);
      manager_->client()->messenger()->scheduler().Schedule(
          std::bind(&Impl::SendHeartbeat, this, TransactionStatus::PENDING, weak_transaction),
          std::chrono::microseconds(FLAGS_transaction_heartbeat_usec));
    } else {
      LOG_WITH_PREFIX(WARNING) << "Send heartbeat failed: " << status;
      if (status.IsExpired()) {
        SetError(status);
        return;
      }
      // Other errors could have different causes, but we should just retry sending heartbeat
      // in this case.
      SendHeartbeat(transaction_status, transaction);
    }
  }

  void SetError(const Status& status) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (error_.ok()) {
      error_ = status;
      state_.store(TransactionState::kAborted, std::memory_order_release);
    }
  }

  void DoPrepareChild(const Status& status,
                      const YBTransactionPtr& transaction,
                      PrepareChildCallback callback,
                      std::unique_lock<std::mutex>* parent_lock) {
    if (!status.ok()) {
      callback(status);
      return;
    }

    std::unique_lock<std::mutex> lock(mutex_, std::defer_lock);
    if (!parent_lock) {
      lock.lock();
    }
    ChildTransactionDataPB data;
    metadata_.ToPB(data.mutable_metadata());
    read_point_.PrepareChildTransactionData(&data);
    callback(data);
  }

  // Manager is created once per service.
  TransactionManager* const manager_;

  // Transaction related to this impl.
  YBTransaction* const transaction_;

  TransactionMetadata metadata_;
  ConsistentReadPoint read_point_;

  std::string log_prefix_;
  std::atomic<bool> requested_status_tablet_{false};
  internal::RemoteTabletPtr status_tablet_;
  internal::RemoteTabletPtr status_tablet_holder_;
  std::atomic<TransactionState> state_{TransactionState::kRunning};
  // Transaction is successfully initialized and ready to process intents.
  const bool child_;
  bool ready_ = false;
  CommitCallback commit_callback_;
  Status error_;
  rpc::Rpcs::Handle heartbeat_handle_;
  rpc::Rpcs::Handle commit_handle_;
  rpc::Rpcs::Handle abort_handle_;

  struct TabletState {
    bool has_parameters = false;

    void ToPB(TransactionInvolvedTabletPB* out) const {
      out->set_has_parameters(has_parameters);
    }

    void MergeFromPB(const TransactionInvolvedTabletPB& source) {
      has_parameters = has_parameters || source.has_parameters();
    }

    std::string ToString() const {
      return Format("{ has_parameters $0 }", has_parameters);
    }
  };

  typedef std::unordered_map<std::string, TabletState> TabletStates;

  std::mutex mutex_;
  TabletStates tablets_;
  std::vector<Waiter> waiters_;
  std::promise<TransactionMetadata> metadata_promise_;
  std::shared_future<TransactionMetadata> metadata_future_;
};

YBTransaction::YBTransaction(TransactionManager* manager,
                             IsolationLevel isolation)
    : impl_(new Impl(manager, this, isolation)) {
}

YBTransaction::YBTransaction(TransactionManager* manager, ChildTransactionData data)
    : impl_(new Impl(manager, this, std::move(data))) {
}

YBTransaction::~YBTransaction() {
}

bool YBTransaction::Prepare(const std::unordered_set<internal::InFlightOpPtr>& ops,
                            Waiter waiter,
                            TransactionMetadata* metadata) {
  return impl_->Prepare(ops, std::move(waiter), metadata);
}

void YBTransaction::Flushed(const internal::InFlightOps& ops, const Status& status) {
  impl_->Flushed(ops, status);
}

void YBTransaction::Commit(CommitCallback callback) {
  impl_->Commit(std::move(callback));
}

const TransactionId& YBTransaction::id() const {
  return impl_->id();
}

const ConsistentReadPoint& YBTransaction::read_point() const {
  return impl_->read_point();
}

ConsistentReadPoint& YBTransaction::read_point() {
  return impl_->read_point();
}

std::future<Status> YBTransaction::CommitFuture() {
  return MakeFuture<Status>([this](auto callback) { impl_->Commit(std::move(callback)); });
}

void YBTransaction::Abort() {
  impl_->Abort();
}

bool YBTransaction::IsRestartRequired() const {
  return impl_->IsRestartRequired();
}

YBTransactionPtr YBTransaction::CreateRestartedTransaction() {
  auto result = impl_->CreateSimilarTransaction();
  impl_->SetupRestart(result->impl_.get());
  return result;
}

void YBTransaction::PrepareChild(PrepareChildCallback callback) {
  return impl_->PrepareChild(std::move(callback));
}

std::future<Result<ChildTransactionDataPB>> YBTransaction::PrepareChildFuture() {
  return MakeFuture<Result<ChildTransactionDataPB>>([this](auto callback) {
      impl_->PrepareChild(std::move(callback));
  });
}

Result<ChildTransactionResultPB> YBTransaction::FinishChild() {
  return impl_->FinishChild();
}

std::shared_future<TransactionMetadata> YBTransaction::TEST_GetMetadata() const {
  return impl_->TEST_GetMetadata();
}

Status YBTransaction::ApplyChildResult(const ChildTransactionResultPB& result) {
  return impl_->ApplyChildResult(result);
}

} // namespace client
} // namespace yb
