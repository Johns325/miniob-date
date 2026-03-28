/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/operator/physical/limit_physical_operator.h"

#include "common/log/log.h"

RC LimitPhysicalOperator::open(Trx *trx)
{
  output_ = 0;
  if (children_.empty()) {
    return RC::SUCCESS;
  }
  return children_[0]->open(trx);
}

RC LimitPhysicalOperator::next()
{
  if (limit_ >= 0 && output_ >= static_cast<size_t>(limit_)) {
    return RC::RECORD_EOF;
  }
  if (children_.empty()) {
    return RC::RECORD_EOF;
  }
  RC rc = children_[0]->next();
  if (rc == RC::SUCCESS) {
    output_++;
  }
  return rc;
}

RC LimitPhysicalOperator::next(Chunk &chunk)
{
  if (children_.empty()) {
    return RC::RECORD_EOF;
  }
  if (limit_ >= 0 && output_ >= static_cast<size_t>(limit_)) {
    return RC::RECORD_EOF;
  }

  RC rc = children_[0]->next(chunk);
  if (OB_FAIL(rc)) {
    return rc;
  }

  const int rows = chunk.rows();
  if (rows <= 0) {
    return rc;
  }

  if (limit_ < 0) {
    output_ += static_cast<size_t>(rows);
    return rc;
  }

  const size_t remaining = static_cast<size_t>(limit_) - output_;
  if (static_cast<size_t>(rows) <= remaining) {
    output_ += static_cast<size_t>(rows);
    return rc;
  }

  // Exceeded remaining rows: truncate in-place by adjusting column counts.
  const int keep = static_cast<int>(remaining);
  for (int i = 0; i < chunk.column_num(); i++) {
    chunk.column(i).set_count(keep);
  }
  output_ += remaining;
  return rc;
}

Tuple *LimitPhysicalOperator::current_tuple()
{
  if (children_.empty()) {
    return nullptr;
  }
  return children_[0]->current_tuple();
}

RC LimitPhysicalOperator::close()
{
  output_ = 0;
  if (!children_.empty()) {
    return children_[0]->close();
  }
  return RC::SUCCESS;
}

RC LimitPhysicalOperator::tuple_schema(TupleSchema &schema) const
{
  if (children_.empty()) {
    return RC::SUCCESS;
  }
  return children_[0]->tuple_schema(schema);
}
