/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/operator/physical/topn_physical_operator.h"

#include <algorithm>

#include "common/log/log.h"

RC TopNPhysicalOperator::open(Trx *trx)
{
  if (children_.empty()) {
    return RC::SUCCESS;
  }

  RC rc = children_[0]->open(trx);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to open child operator. rc=%s", strrc(rc));
    return rc;
  }

  rows_.clear();
  cursor_ = 0;

  if (limit_ == 0) {
    return RC::SUCCESS;
  }

  vector<SortedRow> heap;
  if (limit_ > 0) {
    heap.reserve(static_cast<size_t>(limit_) + 1);
  }

  auto better = [&](const SortedRow &a, const SortedRow &b) -> bool {
    int cmp = compare_keys(a, b);
    if (cmp != 0) {
      return cmp < 0;
    }
    // Stable for ties: prefer physical row order (RID) when available; else preserve child output order.
    if (a.has_rid && b.has_rid) {
      return RID::compare(&a.rid, &b.rid) < 0;
    }
    return a.seq < b.seq;
  };

  while (RC::SUCCESS == (rc = children_[0]->next())) {
    Tuple *child_tuple = children_[0]->current_tuple();
    if (child_tuple == nullptr) {
      rc = RC::INTERNAL;
      LOG_WARN("child tuple is null");
      break;
    }

    SortedRow row;
    row.seq = heap.size() + rows_.size();
    if (auto *row_tuple = dynamic_cast<RowTuple *>(child_tuple); row_tuple != nullptr) {
      row.rid     = row_tuple->record().rid();
      row.has_rid = true;
    }

    rc = ValueListTuple::make(*child_tuple, row.tuple);
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to materialize tuple. rc=%s", strrc(rc));
      break;
    }

    row.keys.reserve(order_by_exprs_.size());
    for (const auto &expr : order_by_exprs_) {
      Value key;
      rc = expr->get_value(*child_tuple, key);
      if (OB_FAIL(rc)) {
        LOG_WARN("failed to eval topn order expr. rc=%s", strrc(rc));
        break;
      }
      row.keys.emplace_back(std::move(key));
    }
    if (OB_FAIL(rc)) {
      break;
    }

    if (limit_ < 0) {
      // Should not happen for TopN, but keep semantics safe: behave like ORDER BY.
      rows_.emplace_back(std::move(row));
      continue;
    }

    heap.emplace_back(std::move(row));
    std::push_heap(heap.begin(), heap.end(), better);

    if (static_cast<int>(heap.size()) > limit_) {
      std::pop_heap(heap.begin(), heap.end(), better);
      heap.pop_back();
    }
  }

  if (rc == RC::RECORD_EOF) {
    rc = RC::SUCCESS;
  }
  if (OB_FAIL(rc)) {
    return rc;
  }

  if (limit_ < 0) {
    std::sort(rows_.begin(), rows_.end(), better);
    return RC::SUCCESS;
  }

  // Heap contains best N, but not ordered. Sort to produce correct output order.
  std::sort(heap.begin(), heap.end(), better);
  rows_.swap(heap);

  return RC::SUCCESS;
}

int TopNPhysicalOperator::compare_keys(const SortedRow &lhs, const SortedRow &rhs) const
{
  const size_t key_num = std::min(lhs.keys.size(), rhs.keys.size());
  for (size_t i = 0; i < key_num; i++) {
    int cmp = lhs.keys[i].compare(rhs.keys[i]);
    if (cmp == 0) {
      continue;
    }
    if (i < asc_.size() && asc_[i] == 0) {
      cmp = -cmp;
    }
    return cmp;
  }
  return 0;
}

RC TopNPhysicalOperator::next()
{
  if (cursor_ >= rows_.size()) {
    return RC::RECORD_EOF;
  }
  cursor_++;
  return RC::SUCCESS;
}

Tuple *TopNPhysicalOperator::current_tuple()
{
  if (cursor_ == 0 || cursor_ > rows_.size()) {
    return nullptr;
  }
  return &rows_[cursor_ - 1].tuple;
}

RC TopNPhysicalOperator::close()
{
  rows_.clear();
  cursor_ = 0;
  if (!children_.empty()) {
    return children_[0]->close();
  }
  return RC::SUCCESS;
}

RC TopNPhysicalOperator::tuple_schema(TupleSchema &schema) const
{
  if (children_.empty()) {
    return RC::SUCCESS;
  }
  return children_[0]->tuple_schema(schema);
}
