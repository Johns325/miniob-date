/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/optimizer/cascade/tasks/apply_rule_task.h"
#include "sql/optimizer/cascade/tasks/o_input_task.h"
#include "sql/optimizer/cascade/tasks/o_expr_task.h"
#include "sql/optimizer/cascade/group_expr.h"
#include "sql/optimizer/cascade/rules.h"
#include "common/log/log.h"
#include "sql/optimizer/cascade/memo.h"
#include "sql/optimizer/cascade/group.h"

RC ApplyRule::perform()
{
  LOG_TRACE("ApplyRule::perform() for rule: {%d}", rule_->get_rule_idx());
  if (group_expr_->rule_explored(rule_)) {
    return RC::SUCCESS;
  }
  // TODO: expr binding, currently group_expr_->get_op() is enough
  // TODO: check condition

  std::vector<CandidateExpression> after;
  rule_->transform(group_expr_, &after, context_);
  for (auto &candidate : after) {
    GroupExpr *new_gexpr = nullptr;
    auto g_id = group_expr_->get_group_id();
    // Note: record_node_into_group will not check if the same expression already exists,
    // so we need check it manually.
    if(!checkDuplicate(candidate, g_id) && context_->record_node_into_group(candidate, &new_gexpr, g_id)) {
      if (new_gexpr->get_op()->is_logical()) {
        // further optimize new expr
        push_task(new OptimizeExpression(new_gexpr, context_));
      } else {
        // calculate the cost of the new physical expr
        push_task(new OptimizeInputs(new_gexpr, context_));
      }
    } else {
      LOG_DEBUG("record_operator_node_into_group not insert new expr");
      // new_gexpr->dump();
    }
  }

  group_expr_->set_rule_explored(rule_);
  return RC::SUCCESS;
}

bool ApplyRule::checkDuplicate(const CandidateExpression &candidate, int target_group)
{
  Memo &memo = context_->get_memo();
  auto group = memo.get_group_by_id(target_group);
  auto expressions = candidate.op->is_logical() ? group->get_logical_expressions() : group->get_physical_expressions();
  for (const auto &expr : expressions) {
    if (expr->get_op()->get_op_type() == candidate.op->get_op_type() && expr->get_child_group_ids() == candidate.child_group_ids) {
      return true;
    }
  }
  return false;
}