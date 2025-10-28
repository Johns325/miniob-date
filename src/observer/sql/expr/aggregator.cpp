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

RC MinAggregator::accumulate(const Value &value)
{
  // Lab2 TODO 
  // 实现 聚合函数MIN的accumulate过程。
  // 要点：传入的值和比较收集到的最小值比较，根据比较结果更新最小值。
  // 你在实现完代码后，请删除最后一行代码 return RC::UNIMPLEMENTED;。

  return RC::UNIMPLEMENTED;
}

RC MinAggregator::evaluate(Value& result)
{
   // Lab2 TODO 
  // 实现 聚合函数MIN的evaluate过程。
  // 要点：把收集到的最小值保存到输入参数中。
  // 你在实现完代码后，请删除最后一行代码 return RC::UNIMPLEMENTED;。

  return RC::UNIMPLEMENTED;
}

RC MaxAggregator::accumulate(const Value &value)
{
  // Lab2 TODO 
  // 实现 聚合函数MAX的accumulate过程。
  // 要点：传入的值和比较收集到的最大值比较，根据比较结果更新最大值。
  // 你在实现完代码后，请删除最后一行代码 return RC::UNIMPLEMENTED;。

  return RC::UNIMPLEMENTED;
}

RC MaxAggregator::evaluate(Value& result)
{
  // Lab2 TODO 
  // 实现 聚合函数MAX的evaluate过程。
  // 要点：把收集到的最大值保存到输入参数中。
  // 你在实现完代码后，请删除最后一行代码 return RC::UNIMPLEMENTED;。

  return RC::UNIMPLEMENTED;
}

RC AvgAggregator::accumulate(const Value &value)
{
  // Lab2 TODO 
  // 实现 聚合函数AVG的accumulate过程。
  // 要点：根据传入的值更新当前收集的总和和输入值个数。
  // 你在实现完代码后，请删除最后一行代码 return RC::UNIMPLEMENTED;。

  return RC::UNIMPLEMENTED;
}

RC AvgAggregator::evaluate(Value& result)
{
  // Lab2 TODO 
  // 实现 聚合函数AVG的evaluate过程。
  // 要点：把收集到的最大值保存到输入参数中。
  // 你在实现完代码后，请删除最后一行代码 return RC::UNIMPLEMENTED;。

  return RC::UNIMPLEMENTED;
}

RC CountAggregator::accumulate(const Value &value)
{
  // Lab2 TODO 
  // 实现 聚合函数COUNT的accumulate过程。
  // 要点：根据传入的值更新当前收集的计数值。
  // 你在实现完代码后，请删除最后一行代码 return RC::UNIMPLEMENTED;。

  return RC::UNIMPLEMENTED;
}

RC CountAggregator::evaluate(Value& result)
{
  // Lab2 TODO 
  // 实现 聚合函数COUNT的evaluate过程。
  // 要点：把收集到的计数值保存到输入参数中。
  // 你在实现完代码后，请删除最后一行代码 return RC::UNIMPLEMENTED;。

  return RC::UNIMPLEMENTED;
}