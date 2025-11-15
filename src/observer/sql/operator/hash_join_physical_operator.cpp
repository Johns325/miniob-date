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
  for (auto &child : children_) {
    rc = child->open(trx);
    if (!OB_SUCC(rc)) {
      LOG_ERROR("Failed to open child operator:");
      return rc;
    }
  }
  left_ = children_[0].get();
  right_ = children_[1].get();

  // TODO 确定好 左表的join key
  // TupleSchema left_schema;
  // TupleSchema right_schema;
  // rc = left_->tuple_schema(left_schema);
  // if (!OB_SUCC(rc)) {
  //   LOG_ERROR("Failed to get left child's schema information");
  //   return rc;
  // }
  // rc = right_->tuple_schema(right_schema);
  // if (!OB_SUCC(rc)) {
  //   LOG_ERROR("Failed to get right child's schema information");
  //   return rc;
  // }
  
  std::vector<int> value_positions;
  bool first_emit_{false};
  while (RC::SUCCESS == (rc = left_->next())) {
    auto tuple = left_->current_tuple();
    if (!first_emit_) {
      get_hashkey_positions(tuple,left_key_positions_);
      first_emit_ = true;
    }
    HashKeyNode node;
    extract_hash_keys(tuple, node, left_key_positions_);
    hash_table_.insert({node, left_tuples_.size()});
    Tuple * t{nullptr};
    tuple->copy(t);
    left_tuples_.emplace_back(t);
  }
  if (rc != RC::RECORD_EOF) {
    return rc;
  }
  return RC::SUCCESS;
}
RC HashJoinPhysicalOperator::next() {
  RC rc {RC::SUCCESS};
  while (RC::SUCCESS == (rc = right_->next())) {
    auto tuple = right_->current_tuple();
    if (!right_emited_) {
      get_hashkey_positions(tuple, right_key_positions_);
      right_emited_ = true;
    }
    HashKeyNode node;
    extract_hash_keys(tuple, node, right_key_positions_);
    auto pos = hash_table_.find(node);
    if (pos != hash_table_.end()) {
      left_tuple_ = left_tuples_[pos->second];
      right_tuple_ = tuple;
      joined_tuple_.set_left(left_tuple_);
      joined_tuple_.set_right(right_tuple_);
      return RC::SUCCESS;
    }
  }
  if (RC::RECORD_EOF != rc) {
    return rc;
  }
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
  for (auto& expr : static_cast<ConjunctionExpr*>(join_conditions_.get())->children()) {
    ASSERT(expr->type() == ExprType::COMPARISON, "the condition that pred is comparsion expression must be held.");
    ComparisonExpr* cmp_pred = static_cast<ComparisonExpr*>(expr.get());
    std::unique_ptr<Expression>& lchild = cmp_pred->left();
    std::unique_ptr<Expression>& rchild = cmp_pred->right();
    int index1{-1}, index2{-1};
    if (lchild->type() == ExprType::FIELD) {
      auto field_expr = static_cast<FieldExpr*>(lchild.get());
      TupleCellSpec spec(field_expr->field().table_name(), field_expr->field().field_name());
      if (OB_SUCC(tuple->find_cell(spec, index1))) {
        ASSERT(index1 != -1, "Impossible");
        key_positions.emplace_back(index1);
      }
    }
    if (rchild->type() == ExprType::FIELD) {
      auto field_expr = static_cast<FieldExpr*>(rchild.get());
      TupleCellSpec spec(field_expr->field().table_name(), field_expr->field().field_name());
      if (OB_SUCC(tuple->find_cell(spec, index2))) {
        ASSERT(index2 != -1, "Impossible");
        key_positions.emplace_back(index2);
      }
    }

    if (lchild->type() == ExprType::FIELD && rchild->type() == ExprType::FIELD) {
      //
      if ((index1 != -1 && index2 != -1) || (index1 == -1 && index2 == -1)) {
        // 同时为-1和同时不为-1都不争取 
        return RC::INTERNAL;
      } 
    }
  }
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
  node.keys.resize(left_key_positions.size());
  for (size_t i = 0; i < left_key_positions.size(); i++) {
    rc = tuple->cell_at(left_key_positions[i], node.keys[i]);
    if (!OB_SUCC(rc)) {
      return rc;
    }
  }
  return rc;
}