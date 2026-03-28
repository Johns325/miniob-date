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

/**
 * @brief LIMIT physical operator
 */
class LimitPhysicalOperator : public PhysicalOperator
{
public:
  explicit LimitPhysicalOperator(int limit) : limit_(limit) {}

  ~LimitPhysicalOperator() override = default;

  OpType get_op_type() const override { return OpType::LIMIT; }

  RC open(Trx *trx) override;
  RC next() override;
  RC next(Chunk &chunk) override;
  RC close() override;

  Tuple *current_tuple() override;

  RC tuple_schema(TupleSchema &schema) const override;

private:
  int    limit_  = -1;
  size_t output_ = 0;
};
