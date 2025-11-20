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

#include "common/sys/rc.h"
#include "sql/operator/physical_operator.h"
#include "sql/parser/parse.h"
#include <functional>
#include <cstdint>
#include <memory>
#include <unordered_map>

/**
 * @brief Hash Join 算子
 * @ingroup PhysicalOperator
 */
struct HashKeyNode {
  // LAB3 TODO
  /*
    定义用于存储哈希键的结构体成员
    重载 == 运算符以便 HashKeyNode 可以作为哈希表的键
  */
};
struct HashKeyNodeHasher {
  // LAB3 TODO
  /*
    定义哈希函数以便 HashKeyNode 可以作为哈希表的键
  */
};

class HashJoinPhysicalOperator : public PhysicalOperator
{
public:
  HashJoinPhysicalOperator() = default;
  virtual ~HashJoinPhysicalOperator() = default;

  PhysicalOperatorType type() const override { return PhysicalOperatorType::HASH_JOIN; }

  OpType get_op_type() const override { return OpType::INNERNLJOIN; }

  void set_predicate(unique_ptr<Expression>&& predicate) {
    ASSERT(predicate->type() == ExprType::CONJUNCTION, "predicate should be a conjunction expression");
    join_conditions_ = std::move(predicate);
  }

  virtual double calculate_cost(
      LogicalProperty *prop, const vector<LogicalProperty *> &child_log_props, CostModel *cm) override
  {
    return 0.0;
  }

  RC     open(Trx *trx) override;
  RC     next() override;
  RC     close() override;
  Tuple *current_tuple() override;

private:
  RC left_next();   //! 左表遍历下一条数据
  RC right_next();  //! 右表遍历下一条数据，如果上一轮结束了就重新开始新的一轮
  bool find_position(const TupleSchema& schemas, TupleCellSpec& spec, int& index);
  RC get_hashkey_positions(Tuple* tuple, std::vector<int>& left_key_positions);
  RC extract_hash_keys(Tuple* tuple, HashKeyNode&, std::vector<int>& left_key_positions);

  // TODO: remove this func
  // Expression *predicate() { return predicate_; }

private:
  Trx *trx_ = nullptr;
  bool right_emited_{false};

  //! 左表右表的真实对象是在PhysicalOperator::children_中，这里是为了写的时候更简单
  PhysicalOperator *left_        = nullptr;
  PhysicalOperator *right_       = nullptr;
  Tuple            *left_tuple_  = nullptr;
  Tuple            *right_tuple_ = nullptr;
  JoinedTuple       joined_tuple_;         //! 当前关联的左右两个tuple
  
  std::vector<int> left_key_positions_; // 存储左表哈希键在tuple中的位置
  std::vector<int> right_key_positions_; // 存储右表哈希键在tuple中的位置
  unique_ptr<Expression> join_conditions_; // 连接谓词表达式
  std::vector<Tuple*> left_tuples_; // 存储左表所有的tuple指针
  using hashed_map_t = std::unordered_map<HashKeyNode,size_t, HashKeyNodeHasher>;
  hashed_map_t hash_table_; // 哈希表
};