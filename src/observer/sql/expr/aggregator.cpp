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
// Created by Wangyunlai on 2024/05/29.
//

#include "sql/expr/aggregator.h"
#include "common/log/log.h"

RC SumAggregator::accumulate(const Value &value)
{
  if (value_.attr_type() == AttrType::UNDEFINED) {
    value_ = value;
    return RC::SUCCESS;
  }
  
  ASSERT(value.attr_type() == value_.attr_type(), "type mismatch. value type: %s, value_.type: %s", 
        attr_type_to_string(value.attr_type()), attr_type_to_string(value_.attr_type()));
  
  Value::add(value, value_, value_);
  return RC::SUCCESS;
}

RC SumAggregator::evaluate(Value& result)
{
  result = value_;
  return RC::SUCCESS;
}

RC CountAggregator::accumulate(const Value &value)
{
  // MiniOB currently doesn't have a dedicated NULL value type.
  // Treat UNDEFINED as "no value" and skip it.
  if (value.attr_type() != AttrType::UNDEFINED) {
    count_++;
  }
  return RC::SUCCESS;
}

RC CountAggregator::evaluate(Value &result)
{
  result.set_int(count_);
  return RC::SUCCESS;
}

RC AvgAggregator::accumulate(const Value &value)
{
  if (value.attr_type() == AttrType::UNDEFINED) {
    return RC::SUCCESS;
  }

  if (value.attr_type() == AttrType::INTS) {
    sum_ += value.get_int();
  } else if (value.attr_type() == AttrType::FLOATS) {
    sum_ += value.get_float();
  } else {
    LOG_WARN("unsupported value type for avg aggregator: %s", attr_type_to_string(value.attr_type()));
    return RC::INVALID_ARGUMENT;
  }

  count_++;
  return RC::SUCCESS;
}

RC AvgAggregator::evaluate(Value &result)
{
  // The operator only evaluates aggregators for non-empty inputs.
  // Keep it safe anyway.
  if (count_ <= 0) {
    result.set_float(0);
    return RC::SUCCESS;
  }

  result.set_float(static_cast<float>(sum_ / static_cast<double>(count_)));
  return RC::SUCCESS;
}

RC MinAggregator::accumulate(const Value &value)
{
  if (value.attr_type() == AttrType::UNDEFINED) {
    return RC::SUCCESS;
  }

  if (value_.attr_type() == AttrType::UNDEFINED) {
    value_ = value;
    return RC::SUCCESS;
  }

  ASSERT(value.attr_type() == value_.attr_type(),
      "type mismatch. value type: %s, value_.type: %s",
      attr_type_to_string(value.attr_type()),
      attr_type_to_string(value_.attr_type()));

  if (value.compare(value_) < 0) {
    value_ = value;
  }
  return RC::SUCCESS;
}

RC MinAggregator::evaluate(Value &result)
{
  result = value_;
  return RC::SUCCESS;
}

RC MaxAggregator::accumulate(const Value &value)
{
  if (value.attr_type() == AttrType::UNDEFINED) {
    return RC::SUCCESS;
  }

  if (value_.attr_type() == AttrType::UNDEFINED) {
    value_ = value;
    return RC::SUCCESS;
  }

  ASSERT(value.attr_type() == value_.attr_type(),
      "type mismatch. value type: %s, value_.type: %s",
      attr_type_to_string(value.attr_type()),
      attr_type_to_string(value_.attr_type()));

  if (value.compare(value_) > 0) {
    value_ = value;
  }
  return RC::SUCCESS;
}

RC MaxAggregator::evaluate(Value &result)
{
  result = value_;
  return RC::SUCCESS;
}
