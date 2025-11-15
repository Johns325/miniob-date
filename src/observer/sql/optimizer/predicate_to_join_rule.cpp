/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/optimizer/predicate_to_join_rule.h"
#include "sql/operator/logical_operator.h"
#include "sql/operator/predicate_logical_operator.h"
#include "sql/operator/table_get_logical_operator.h"

// void PredicateToJoinRewriter::visitor (LogicalOperator* oper, std::vector<TableGetLogicalOperator*>& table_get_ops) {
//   if (oper == nullptr) {
//     return;
//   }
//   if (oper->type() == LogicalOperatorType::TABLE_GET) {
//     table_get_ops.emplace_back(dynamic_cast<TableGetLogicalOperator*>(oper));
//     return;
//   }
//   for (auto& child : oper->children()) {
//     visitor(child.get(), table_get_ops);
//   }
// };
// RC PredicateToJoinRewriter::rewrite(unique_ptr<LogicalOperator> &oper, bool &change_made) {
//   if (oper->type() != LogicalOperatorType::PREDICATE) {
//     return RC::SUCCESS;
//   }
//   auto pred_oper = dynamic_cast<PredicateLogicalOperator*>(oper.get());
//   std::vector<TableGetLogicalOperator*> table_get_ops;
//   std::vector<bool> valid(pred_oper->expressions().size(), true);
//   visitor(oper.get(), table_get_ops);
//   for (auto& expr : pred_oper->expressions()) {
//     if (expr->type() != ExprType::COMPARISON) {
//       continue;
//     }
//     auto cmp_expr = dynamic_cast<ComparisonExpr*>(expr.get());
//     if (!cmp_expr->field_value_comparison()) {
//       continue;
//     }
//     auto field_expr = dynamic_cast<FieldExpr*>(cmp_expr->left()->type() == ExprType::FIELD ? cmp_expr->left().get(): cmp_expr->right().get());
//     auto tb_name = string(field_expr->table_name());
//     for (auto oper : table_get_ops) {
//       if (tb_name == string(oper->table()->name())) {
//         oper->add_expressions(std::move(expr));
//       }
//     }
//   }
  
//   return RC::SUCCESS;
// }