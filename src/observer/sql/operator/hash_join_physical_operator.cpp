/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/operator/hash_join_physical_operator.h"
#include "common/sys/rc.h"
#include <unordered_map>

RC HashJoinPhysicalOperator::open(Trx *trx) {
  RC rc{RC::SUCCESS};
  if (children_.size() != 2) {
    LOG_WARN("Hash join operator should have 2 children");
    return RC::INTERNAL;
  }
  // LAB3 TODO
  /*
    初始化各种成员变量
    实现 HashJoin 的 Build 阶段
  */
  return RC::SUCCESS;
}
RC HashJoinPhysicalOperator::next() {
  RC rc {RC::SUCCESS};
  // LAB3 TODO
  /*
    实现 HashJoin 的 Probe 阶段
    当找到满足连接条件的左右表记录时，设置 joined_tuple_ 并返回 RC::SUCCESS
    如果没有更多记录可供连接，返回 RC::RECORD_EOF
  */
  return RC::RECORD_EOF;
}
RC HashJoinPhysicalOperator::close() {
  for (auto & ptr : left_tuples_)
    delete ptr;
  
  return RC::SUCCESS;
}
Tuple * HashJoinPhysicalOperator::current_tuple() {
  return &joined_tuple_;
  return nullptr;
}

RC HashJoinPhysicalOperator::get_hashkey_positions(Tuple *tuple, std::vector<int>& key_positions) {
  // LAB3 TODO
  /*
    根据 join_conditions_ 提取哈希键在 tuple 中的位置，填充到 key_positions 中
  */
  return RC::SUCCESS;
}
bool HashJoinPhysicalOperator::find_position(const TupleSchema& schemas, TupleCellSpec& spec, int& index) {
  for (int i = 0; i < schemas.cell_num(); i++) {
    if (spec.equals(schemas.cell_at(i))) {
      index = i;
      return true;
    }
  }
  return false;
}

RC HashJoinPhysicalOperator::extract_hash_keys(Tuple* tuple, HashKeyNode& node, std::vector<int>& left_key_positions) {
  RC rc{RC::SUCCESS};
  // LAB3 TODO
  /*
    根据 left_key_positions 从 tuple 中提取哈希键值，填充到 node 中
  */
  return rc;
}