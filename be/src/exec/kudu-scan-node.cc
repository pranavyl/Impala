// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "exec/kudu-scan-node.h"

#include <thrift/protocol/TDebugProtocol.h>

#include "exec/kudu-scanner.h"
#include "exec/kudu-util.h"
#include "exprs/scalar-expr.h"
#include "gutil/gscoped_ptr.h"
#include "runtime/fragment-instance-state.h"
#include "runtime/mem-pool.h"
#include "runtime/runtime-state.h"
#include "runtime/row-batch.h"
#include "runtime/thread-resource-mgr.h"
#include "runtime/tuple-row.h"
#include "util/runtime-profile-counters.h"

#include "common/names.h"

DEFINE_int32(kudu_max_row_batches, 0, "The maximum size of the row batch queue, "
    " for Kudu scanners.");

namespace impala {

KuduScanNode::KuduScanNode(ObjectPool* pool, const TPlanNode& tnode,
    const DescriptorTbl& descs)
    : KuduScanNodeBase(pool, tnode, descs),
      done_(false),
      thread_avail_cb_id_(-1) {
  DCHECK(KuduIsAvailable());
}

KuduScanNode::~KuduScanNode() {
  DCHECK(is_closed());
}

Status KuduScanNode::Prepare(RuntimeState* state) {
  RETURN_IF_ERROR(KuduScanNodeBase::Prepare(state));
  thread_state_.Prepare(this);
  return Status::OK();
}

Status KuduScanNode::Open(RuntimeState* state) {
  SCOPED_TIMER(runtime_profile_->total_time_counter());
  RETURN_IF_ERROR(KuduScanNodeBase::Open(state));
  thread_state_.Open(this, FLAGS_kudu_max_row_batches);

  if (filter_ctxs_.size() > 0) WaitForRuntimeFilters();

  thread_avail_cb_id_ = state->resource_pool()->AddThreadAvailableCb(
      bind<void>(mem_fn(&KuduScanNode::ThreadAvailableCb), this, _1));
  ThreadAvailableCb(state->resource_pool());
  return Status::OK();
}

Status KuduScanNode::GetNext(RuntimeState* state, RowBatch* row_batch, bool* eos) {
  DCHECK(row_batch != NULL);
  RETURN_IF_ERROR(ExecDebugAction(TExecNodePhase::GETNEXT, state));
  RETURN_IF_CANCELLED(state);
  RETURN_IF_ERROR(QueryMaintenance(state));
  SCOPED_TIMER(runtime_profile_->total_time_counter());
  SCOPED_TIMER(materialize_tuple_timer());

  // If there are no scan tokens, nothing is ever placed in the materialized
  // row batch, so exit early for this case.
  if (ReachedLimit() || NumScanTokens() == 0) {
    *eos = true;
    return Status::OK();
  }

  *eos = false;
  unique_ptr<RowBatch> materialized_batch = thread_state_.batch_queue()->GetBatch();
  if (materialized_batch != NULL) {
    row_batch->AcquireState(materialized_batch.get());
    num_rows_returned_ += row_batch->num_rows();
    COUNTER_SET(rows_returned_counter_, num_rows_returned_);

    if (ReachedLimit()) {
      int num_rows_over = num_rows_returned_ - limit_;
      row_batch->set_num_rows(row_batch->num_rows() - num_rows_over);
      num_rows_returned_ -= num_rows_over;
      COUNTER_SET(rows_returned_counter_, num_rows_returned_);
      *eos = true;

      SetDone();
    }
    materialized_batch.reset();
  } else {
    *eos = true;
  }

  unique_lock<mutex> l(lock_);
  return status_;
}

void KuduScanNode::Close(RuntimeState* state) {
  if (is_closed()) return;
  SCOPED_TIMER(runtime_profile_->total_time_counter());
  if (thread_avail_cb_id_ != -1) {
    state->resource_pool()->RemoveThreadAvailableCb(thread_avail_cb_id_);
  }

  SetDone();

  thread_state_.Close();
  KuduScanNodeBase::Close(state);
}

void KuduScanNode::ThreadAvailableCb(ThreadResourcePool* pool) {
  while (true) {
    unique_lock<mutex> lock(lock_);
    // All done or all tokens are assigned.
    if (done_ || !HasScanToken()) break;
    bool first_thread = thread_state_.GetNumActive() == 0;

    // Check if we can get a token. We need at least one thread to run.
    if (first_thread) {
      pool->AcquireThreadToken();
    } else if (thread_state_.GetNumActive() >= thread_state_.max_num_scanner_threads()
        || !pool->TryAcquireThreadToken()) {
      break;
    }

    string name = Substitute(
        "kudu-scanner-thread (finst:$0, plan-node-id:$1, thread-idx:$2)",
        PrintId(runtime_state_->fragment_instance_id()), id(),
        thread_state_.GetNumStarted());

    // Reserve the first token so no other thread picks it up.
    const string* token = GetNextScanToken();
    auto fn = [this, first_thread, token, name]() {
      this->RunScannerThread(first_thread, name, token);
    };
    std::unique_ptr<Thread> t;
    Status status =
      Thread::Create(FragmentInstanceState::FINST_THREAD_GROUP_NAME, name, fn, &t, true);
    if (!status.ok()) {
      // Release the token and skip running callbacks to find a replacement. Skipping
      // serves two purposes. First, it prevents a mutual recursion between this function
      // and ReleaseThreadToken()->InvokeCallbacks(). Second, Thread::Create() failed and
      // is likely to continue failing for future callbacks.
      pool->ReleaseThreadToken(first_thread, true);

      // Abort the query. This is still holding the lock_, so done_ is known to be
      // false and status_ must be ok.
      DCHECK(status_.ok());
      status_ = status;
      SetDoneInternal();
      break;
    }
    // Thread successfully started
    thread_state_.AddThread(move(t));
  }
}

Status KuduScanNode::ProcessScanToken(KuduScanner* scanner, const string& scan_token) {
  bool eos;
  RETURN_IF_ERROR(scanner->OpenNextScanToken(scan_token, &eos));
  if (eos) return Status::OK();
  while (!eos && !done_) {
    unique_ptr<RowBatch> row_batch = std::make_unique<RowBatch>(row_desc(),
        runtime_state_->batch_size(), mem_tracker());
    RETURN_IF_ERROR(scanner->GetNext(row_batch.get(), &eos));
    while (!done_) {
      scanner->KeepKuduScannerAlive();
      if (thread_state_.EnqueueBatchWithTimeout(&row_batch, 1000000)) {
        break;
      }
      // Make sure that we still own the RowBatch if BlockingPutWithTimeout() timed out.
      DCHECK(row_batch != nullptr);
    }
  }
  if (eos) scan_ranges_complete_counter_->Add(1);
  return Status::OK();
}

void KuduScanNode::RunScannerThread(
    bool first_thread, const string& name, const string* initial_token) {
  DCHECK(initial_token != nullptr);
  SCOPED_THREAD_COUNTER_MEASUREMENT(thread_state_.thread_counters());
  SCOPED_THREAD_COUNTER_MEASUREMENT(runtime_state_->total_thread_statistics());
  KuduScanner scanner(this, runtime_state_);

  const string* scan_token = initial_token;
  Status status = scanner.Open();
  if (status.ok()) {
    // Here, even though a read of 'done_' may conflict with a write to it,
    // ProcessScanToken() will return early, as will GetNextScanToken().
    while (!done_ && scan_token != nullptr) {
      status = ProcessScanToken(&scanner, *scan_token);
      if (!status.ok()) break;

      // Check if we have enough thread tokens to keep using this optional thread. This
      // check is racy: multiple threads may notice that the optional tokens are exceeded
      // and shut themselves down. If we shut down too many and there are more optional
      // tokens, ThreadAvailableCb() will be invoked again.
      if (!first_thread && runtime_state_->resource_pool()->optional_exceeded()) break;

      unique_lock<mutex> l(lock_);
      if (!done_) {
        scan_token = GetNextScanToken();
      } else {
        scan_token = nullptr;
      }
    }
  }
  scanner.Close();

  {
    unique_lock<mutex> l(lock_);
    if (!status.ok() && status_.ok()) {
      status_ = status;
      SetDoneInternal();
    }
    if (thread_state_.DecrementNumActive()) SetDoneInternal();
  }

  // lock_ is released before calling ThreadResourceMgr::ReleaseThreadToken() which
  // invokes ThreadAvailableCb() which attempts to take the same lock.
  VLOG_RPC << "Thread done: " << name;
  runtime_state_->resource_pool()->ReleaseThreadToken(first_thread);
}

void KuduScanNode::SetDoneInternal() {
  if (done_) return;
  done_ = true;
  thread_state_.Shutdown();
}

void KuduScanNode::SetDone() {
  unique_lock<mutex> l(lock_);
  SetDoneInternal();
}

}  // namespace impala
