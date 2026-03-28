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

#include "sql/operator/logical_operator.h"

class OrderByLogicalOperator : public LogicalOperator
{
public:
  OrderByLogicalOperator(vector<unique_ptr<Expression>> &&order_by_exprs, vector<bool> &&asc)
  {
    expressions_ = std::move(order_by_exprs);
    asc_         = std::move(asc);
  }

  ~OrderByLogicalOperator() override = default;

  OpType get_op_type() const override { return OpType::LOGICALORDERBY; }

  const vector<bool> &asc() const { return asc_; }
  vector<bool>       &asc() { return asc_; }

  unique_ptr<LogicalOperator> clone() const override
  {
    vector<unique_ptr<Expression>> order_exprs;
    order_exprs.reserve(expressions_.size());
    for (const auto &expr : expressions_) {
      order_exprs.emplace_back(expr->copy());
    }
    vector<bool> asc = asc_;
    return make_unique<OrderByLogicalOperator>(std::move(order_exprs), std::move(asc));
  }

  uint64_t hash() const override
  {
    uint64_t hash = std::hash<int>()(static_cast<int>(get_op_type()));
    hash ^= std::hash<size_t>()(expressions_.size());
    hash ^= std::hash<size_t>()(asc_.size());
    for (size_t i = 0; i < expressions_.size(); i++) {
      hash ^= std::hash<int>()(static_cast<int>(expressions_[i]->type()));
      hash ^= std::hash<int>()(asc_[i] ? 1 : 0);
    }
    return hash;
  }

  bool operator==(const OperatorNode &other) const override
  {
    if (get_op_type() != other.get_op_type()) {
      return false;
    }
    const auto &o = static_cast<const OrderByLogicalOperator &>(other);
    if (expressions_.size() != o.expressions_.size() || asc_.size() != o.asc_.size()) {
      return false;
    }
    for (size_t i = 0; i < expressions_.size(); i++) {
      if (!expressions_[i]->equal(*o.expressions_[i])) {
        return false;
      }
      if (asc_[i] != o.asc_[i]) {
        return false;
      }
    }
    return true;
  }

private:
  vector<bool> asc_;
};
