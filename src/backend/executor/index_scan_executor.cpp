//===----------------------------------------------------------------------===//
//
//                         PelotonDB
//
// index_scan_executor.cpp
//
// Identification: src/backend/executor/index_scan_executor.cpp
//
// Copyright (c) 2015, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include "backend/executor/index_scan_executor.h"

#include <memory>
#include <utility>
#include <vector>

#include "backend/common/types.h"
#include "backend/executor/logical_tile.h"
#include "backend/executor/logical_tile_factory.h"
#include "backend/expression/abstract_expression.h"
#include "backend/expression/container_tuple.h"
#include "backend/storage/data_table.h"
#include "backend/storage/tile_group.h"

#include "backend/common/logger.h"

namespace peloton {
namespace executor {

/**
 * @brief Constructor for indexscan executor.
 * @param node Indexscan node corresponding to this executor.
 */
IndexScanExecutor::IndexScanExecutor(planner::AbstractPlanNode *node,
                                     ExecutorContext *executor_context)
    : AbstractScanExecutor(node, executor_context) {}

/**
 * @brief Let base class Dinit() first, then do my job.
 * @return true on success, false otherwise.
 */
bool IndexScanExecutor::DInit() {
  auto status = AbstractScanExecutor::DInit();

  if (!status) return false;

  assert(children_.size() == 0);
  LOG_TRACE("Index Scan executor :: 0 child");

  // Grab info from plan node and check it
  const planner::IndexScanNode &node = GetPlanNode<planner::IndexScanNode>();

  index_ = node.GetIndex();
  assert(index_ != nullptr);

  result_itr = START_OID;
  done_ = false;

  column_ids_ = node.GetColumnIds();
  key_column_ids_ = node.GetKeyColumnIds();
  expr_types_ = node.GetExprTypes();
  values_ = node.GetValues();
  runtime_keys_ = node.GetRunTimeKeys();

  if (runtime_keys_.size() != 0) {
    assert(runtime_keys_.size() == values_.size());

    if (!key_ready) {
      values_.clear();

      for (auto expr : runtime_keys_) {
        auto value = expr->Evaluate(nullptr, nullptr, executor_context_);
        LOG_INFO("Evaluated runtime scan key: %s", value.GetInfo().c_str());
        values_.push_back(value);
      }

      key_ready = true;
    }
  }

  auto table = node.GetTable();

  if (table != nullptr) {
    if (column_ids_.empty()) {
      column_ids_.resize(table->GetSchema()->GetColumnCount());
      std::iota(column_ids_.begin(), column_ids_.end(), 0);
    }
  }

  return true;
}

/**
 * @brief Creates logical tile(s) after scanning index.
 * @return true on success, false otherwise.
 */
bool IndexScanExecutor::DExecute() {
  if (!done_) {
    auto status = ExecIndexLookup();
    if (status == false) return false;
  }

  // Already performed the index lookup
  assert(done_);

  while (result_itr < result.size()) {  // Avoid returning empty tiles
    // In order to be as lazy as possible,
    // the generic predicate is checked here (instead of upfront)
    if (nullptr != predicate_) {
      for (oid_t tuple_id : *result[result_itr]) {
        expression::ContainerTuple<LogicalTile> tuple(result[result_itr],
                                                      tuple_id);
        if (predicate_->Evaluate(&tuple, nullptr, executor_context_)
                .IsFalse()) {
          result[result_itr]->RemoveVisibility(tuple_id);
        }
      }
    }

    if (result[result_itr]->GetTupleCount() == 0) {
      result_itr++;
      continue;
    } else {
      SetOutput(result[result_itr]);
      result_itr++;
      return true;
    }

  }  // end while

  return false;
}

bool IndexScanExecutor::ExecIndexLookup() {
  assert(!done_);

  std::vector<ItemPointer> tuple_locations;

  if (0 == key_column_ids_.size()) {
    tuple_locations = index_->Scan();
  } else {
    tuple_locations = index_->Scan(values_, key_column_ids_, expr_types_);
  }

  LOG_INFO("Tuple locations : %lu", tuple_locations.size());

  if (tuple_locations.size() == 0) return false;

  auto transaction_ = executor_context_->GetTransaction();
  txn_id_t txn_id = transaction_->GetTransactionId();
  cid_t commit_id = transaction_->GetLastCommitId();

  // Get the logical tiles corresponding to the given tuple locations
  result = LogicalTileFactory::WrapTileGroups(tuple_locations, column_ids_,
                                              txn_id, commit_id);
  done_ = true;

  LOG_TRACE("Result tiles : %lu", result.size());

  return true;
}

}  // namespace executor
}  // namespace peloton
