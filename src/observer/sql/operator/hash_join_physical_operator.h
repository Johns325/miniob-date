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
  std::vector<Value> keys;
  bool operator==(const HashKeyNode& other) const {
    if (keys.size() != other.keys.size()) {
      return false;
    }
    for (size_t i = 0; i < keys.size(); i++) {
      if (keys[i].compare(other.keys[i]) != 0) {
        return false;
      }
    }
    return true;
  }
};
struct HashKeyNodeHasher {
  std::size_t operator()(const HashKeyNode& node) const {
    // computes the hash of an employee using a variant
    // of the Fowler-Noll-Vo hash function
    constexpr std::uint64_t prime{0x100000001B3};
    std::uint64_t result{0xcbf29ce484222325};
    for (auto & key : node.keys) {
      switch (key.attr_type()) {
        case AttrType::INTS: {
          result = (result * prime) ^ std::hash<int>{}(key.get_int());
        } break;
        case AttrType::FLOATS: {
          result = (result * prime) ^ std::hash<float>{}(key.get_float());
        } break;
        case AttrType::BOOLEANS: {
          result = (result * prime) ^ std::hash<bool>{}(key.get_boolean());
        } break;
        case AttrType::CHARS: {
          auto str = key.get_string();
          for (size_t i = 0; i < str.size(); ++i)
            result = (result * prime) ^ str[i];
        } break;
        default:
          ASSERT(false, "Unsupported type");
      }
    }
    return result;
  }
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
  
  std::vector<int> left_key_positions_;
  std::vector<int> right_key_positions_;
  unique_ptr<Expression> join_conditions_;
  std::vector<Tuple*> left_tuples_;
  using hashed_map_t = std::unordered_map<HashKeyNode,size_t, HashKeyNodeHasher>;
  hashed_map_t hash_table_;
};