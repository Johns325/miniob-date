/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

//
// Created by WangYunlai on 2022/12/30.
//

#include "sql/operator/physical/nested_loop_join_physical_operator.h"

#include <algorithm>

#include "common/log/log.h"
#include "sql/operator/physical/table_scan_physical_operator.h"
#include "sql/optimizer/statistics/table_statistics.h"

NestedLoopJoinPhysicalOperator::NestedLoopJoinPhysicalOperator() {}

double NestedLoopJoinPhysicalOperator::calculate_cost(
    LogicalProperty *prop, const vector<LogicalProperty *> &child_log_props, CostModel *cm)
{
  if (cm == nullptr) {
    return 0.0;
  }

  int64_t outer_rows = 0;
  int64_t inner_rows = 0;
  if (child_log_props.size() >= 2) {
    if (child_log_props[0] != nullptr) {
      outer_rows = child_log_props[0]->get_card();
    }
    if (child_log_props[1] != nullptr) {
      inner_rows = child_log_props[1]->get_card();
    }
  }

  if (outer_rows <= 0 && children_.size() >= 1 && children_[0] != nullptr) {
    outer_rows = static_cast<int64_t>(children_[0]->estimate_output_size());
  }
  if (inner_rows <= 0 && children_.size() >= 2 && children_[1] != nullptr) {
    inner_rows = static_cast<int64_t>(children_[1]->estimate_output_size());
  }

  // Avoid zero-cost plans.
  if (outer_rows <= 0) {
    outer_rows = 1;
  }
  if (inner_rows <= 0) {
    inner_rows = 1;
  }

  int64_t output_rows = 0;
  // if (prop != nullptr) {
  //   output_rows = prop->get_card();
  // }
  if (output_rows <= 0) {
    output_rows = static_cast<int64_t>(estimate_output_size());
  }
  if (output_rows <= 0) {
    output_rows = 1;
  }

  const double cpu_tuple_cost = cm->cpu_tuple_cost();
  // total_cost = cost_outer + cost_inner + outer_rows*inner_rows*cpu_tuple_cost + output_rows*cpu_tuple_cost
  // Optimizer framework already adds child (outer/inner) costs; this returns the local join cost only.
  return cpu_tuple_cost * (static_cast<double>(outer_rows) * static_cast<double>(inner_rows) + static_cast<double>(output_rows));
}

static int64_t estimate_ndv_from_child(const PhysicalOperator *child, const char *field_name)
{
  if (child == nullptr || field_name == nullptr || *field_name == '\0') {
    return 0;
  }

  auto table_scan = dynamic_cast<const TableScanPhysicalOperator *>(child);
  if (table_scan == nullptr || table_scan->table() == nullptr) {
    return 0;
  }

  TableStatistics stats;
  if (stats.analyze(table_scan->table(), nullptr) != RC::SUCCESS) {
    return 0;
  }

  int64_t ndv = 0;
  if (stats.get_ndv(field_name, ndv) != RC::SUCCESS) {
    return 0;
  }
  return ndv;
}

size_t NestedLoopJoinPhysicalOperator::estimate_output_size() const
{
  if (children_.size() != 2) {
    return 0;
  }

  const PhysicalOperator *left  = children_[0].get();
  const PhysicalOperator *right = children_[1].get();
  const int64_t           t_left  = static_cast<int64_t>(left ? left->estimate_output_size() : 0);
  const int64_t           t_right = static_cast<int64_t>(right ? right->estimate_output_size() : 0);
  if (t_left <= 0 || t_right <= 0) {
    return 0;
  }

  // Try to find an equi-join predicate: FieldExpr = FieldExpr
  const FieldExpr *left_field  = nullptr;
  const FieldExpr *right_field = nullptr;
  for (const auto &pred : join_predicates_) {
    if (!pred || pred->type() != ExprType::COMPARISON) {
      continue;
    }
    const auto *cmp = static_cast<const ComparisonExpr *>(pred.get());
    if (cmp->comp() != CompOp::EQUAL_TO) {
      continue;
    }
    if (!cmp->left() || !cmp->right()) {
      continue;
    }
    if (cmp->left()->type() == ExprType::FIELD && cmp->right()->type() == ExprType::FIELD) {
      left_field  = static_cast<const FieldExpr *>(cmp->left().get());
      right_field = static_cast<const FieldExpr *>(cmp->right().get());
      break;
    }
  }

  int64_t v_left  = 0;
  int64_t v_right = 0;
  if (left_field != nullptr && right_field != nullptr) {
    v_left  = estimate_ndv_from_child(left, left_field->field_name());
    v_right = estimate_ndv_from_child(right, right_field->field_name());
  }

  // Fallbacks: if NDV not available, use child cardinality as a safe upper bound.
  if (v_left <= 0) {
    v_left = t_left;
  }
  if (v_right <= 0) {
    v_right = t_right;
  }

  const int64_t denom = std::max<int64_t>(1, std::max(v_left, v_right));
  const long double est = (static_cast<long double>(t_left) * static_cast<long double>(t_right)) /
                          static_cast<long double>(denom);
  if (est <= 0) {
    return 0;
  }
  if (est > static_cast<long double>(std::numeric_limits<size_t>::max())) {
    return std::numeric_limits<size_t>::max();
  }
  return static_cast<size_t>(est);
}

bool NestedLoopJoinPhysicalOperator::eval_join_predicates(const Tuple &left_tuple, const Tuple &right_tuple) const
{
  if (join_predicates_.empty()) {
    return true;
  }

  JoinedTuple tmp_joined;
  tmp_joined.set_left(const_cast<Tuple *>(&left_tuple));
  tmp_joined.set_right(const_cast<Tuple *>(&right_tuple));

  for (const auto &pred : join_predicates_) {
    if (!pred) {
      continue;
    }
    Value pred_val;
    RC    rc = pred->get_value(tmp_joined, pred_val);
    if (rc != RC::SUCCESS) {
      return false;
    }
    if (pred_val.attr_type() != AttrType::BOOLEANS) {
      return false;
    }
    if (!pred_val.get_boolean()) {
      return false;
    }
  }
  return true;
}

RC NestedLoopJoinPhysicalOperator::open(Trx *trx)
{
  if (children_.size() != 2) {
    LOG_WARN("nlj operator should have 2 children");
    return RC::INTERNAL;
  }

  RC rc         = RC::SUCCESS;
  left_         = children_[0].get();
  right_        = children_[1].get();
  right_closed_ = true;
  round_done_   = true;

  rc   = left_->open(trx);
  trx_ = trx;
  return rc;
}

RC NestedLoopJoinPhysicalOperator::next()
{
  while (true) {
    // Need a new left tuple (first time or after finishing an inner scan)
    if (left_tuple_ == nullptr || round_done_) {
      RC rc = left_next();
      if (rc != RC::SUCCESS) {
        return rc;
      }
      // Start a new inner scan for the new left tuple
      round_done_ = true;
    }

    RC rc = right_next();
    if (rc == RC::SUCCESS) {
      // right_next sets right_tuple_ and updates joined_tuple_. Filter by join predicates.
      if (left_tuple_ != nullptr && right_tuple_ != nullptr && eval_join_predicates(*left_tuple_, *right_tuple_)) {
        return RC::SUCCESS;
      }
      // predicate failed, continue scanning right tuples for the same left tuple
      continue;
    }
    if (rc == RC::RECORD_EOF) {
      // Finished one inner scan, advance left in the next iteration.
      left_tuple_ = nullptr;
      continue;
    }
    return rc;
  }
}

RC NestedLoopJoinPhysicalOperator::close()
{
  RC rc = left_->close();
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to close left oper. rc=%s", strrc(rc));
  }

  if (!right_closed_) {
    rc = right_->close();
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to close right oper. rc=%s", strrc(rc));
    } else {
      right_closed_ = true;
    }
  }
  return rc;
}

Tuple *NestedLoopJoinPhysicalOperator::current_tuple() { return &joined_tuple_; }

RC NestedLoopJoinPhysicalOperator::left_next()
{
  RC rc = RC::SUCCESS;
  rc    = left_->next();
  if (rc != RC::SUCCESS) {
    return rc;
  }

  left_tuple_ = left_->current_tuple();
  joined_tuple_.set_left(left_tuple_);
  return rc;
}

RC NestedLoopJoinPhysicalOperator::right_next()
{
  RC rc = RC::SUCCESS;
  if (round_done_) {
    if (!right_closed_) {
      rc = right_->close();

      right_closed_ = true;
      if (rc != RC::SUCCESS) {
        return rc;
      }
    }

    rc = right_->open(trx_);
    if (rc != RC::SUCCESS) {
      return rc;
    }
    right_closed_ = false;

    round_done_ = false;
  }

  rc = right_->next();
  if (rc != RC::SUCCESS) {
    if (rc == RC::RECORD_EOF) {
      round_done_ = true;
    }
    return rc;
  }

  right_tuple_ = right_->current_tuple();
  joined_tuple_.set_right(right_tuple_);
  return rc;
}
