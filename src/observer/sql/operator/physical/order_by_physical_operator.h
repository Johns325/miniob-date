/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once

#include "sql/operator/physical_operator.h"

#include "sql/expr/expression.h"
#include "sql/expr/tuple.h"
#include "storage/record/record.h"

/**
 * @brief ORDER BY physical operator (tuple iterator mode)
 */
class OrderByPhysicalOperator : public PhysicalOperator
{
public:
  OrderByPhysicalOperator(vector<unique_ptr<Expression>> &&order_by_exprs, vector<bool> &&asc)
      : order_by_exprs_(std::move(order_by_exprs))
  {
    asc_.reserve(asc.size());
    for (bool v : asc) {
      asc_.push_back(static_cast<uint8_t>(v ? 1 : 0));
    }
  }

  ~OrderByPhysicalOperator() override = default;

  OpType get_op_type() const override { return OpType::ORDERBY; }

  RC open(Trx *trx) override;
  RC next() override;
  RC close() override;

  Tuple *current_tuple() override;

  RC tuple_schema(TupleSchema &schema) const override;

private:
  struct SortedRow
  {
    ValueListTuple tuple;
    vector<Value>  keys;
    size_t         seq = 0;
    RID            rid{};
    bool           has_rid = false;
  };

  int compare_keys(const SortedRow &lhs, const SortedRow &rhs) const;

private:
  vector<unique_ptr<Expression>> order_by_exprs_;
  vector<uint8_t>               asc_;

  vector<SortedRow> rows_;
  size_t            cursor_ = 0;
};
