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

class LimitLogicalOperator : public LogicalOperator
{
public:
  explicit LimitLogicalOperator(int limit) : limit_(limit) {}

  ~LimitLogicalOperator() override = default;

  OpType get_op_type() const override { return OpType::LOGICALLIMIT; }

  int limit() const { return limit_; }

  unique_ptr<LogicalOperator> clone() const override { return make_unique<LimitLogicalOperator>(limit_); }

  uint64_t hash() const override
  {
    uint64_t hash = std::hash<int>()(static_cast<int>(get_op_type()));
    hash ^= std::hash<int>()(limit_);
    return hash;
  }

  bool operator==(const OperatorNode &other) const override
  {
    if (get_op_type() != other.get_op_type()) {
      return false;
    }
    const auto &o = static_cast<const LimitLogicalOperator &>(other);
    return limit_ == o.limit_;
  }

  string param() const override { return std::to_string(limit_); }

private:
  int limit_ = -1;
};
