/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/optimizer/cascade/transformation_rules.h"
#include "common/log/log.h"
#include "sql/optimizer/cascade/group_expr.h"
#include "sql/optimizer/cascade/memo.h"
#include "sql/operator/logical/predicate_logical_operator.h"
#include "sql/operator/logical/table_get_logical_operator.h"
#include "sql/operator/logical/join_logical_operator.h"
#include "sql/operator/logical/empty_logical_operator.h"
#include "sql/expr/expression.h"

// -------------------------------------------------------------------------------------------------
// PredicatePushdownRule
// -------------------------------------------------------------------------------------------------
PredicatePushdownRule::PredicatePushdownRule()
{
  type_          = RuleType::PREDICATE_PUSHDOWN;
  match_pattern_ = unique_ptr<Pattern>(new Pattern(OpType::LOGICALFILTER));
  auto child     = new Pattern(OpType::LOGICALGET);
  match_pattern_->add_child(child);
}

void PredicatePushdownRule::transform(
    GroupExpr *input, std::vector<CandidateExpression> *transformed, OptimizerContext *context) const
{
  ASSERT(input->get_children_groups_size() == 1, "Filter should have 1 child");

  auto predicate_oper = static_cast<PredicateLogicalOperator *>(input->get_op());
  vector<unique_ptr<Expression>> &expressions = predicate_oper->expressions();
  if (expressions.size() != 1) {
    return;
  }

  // Get child GroupExpr
  Memo &memo = context->get_memo();
  Group *child_group = memo.get_group_by_id(input->get_child_group_ids()[0]);
  GroupExpr *child_gexpr = child_group->get_logical_expression();
  if (!child_gexpr || child_gexpr->get_op()->get_op_type() != OpType::LOGICALGET) {
    return;
  }

  auto table_get_oper = static_cast<TableGetLogicalOperator *>(child_gexpr->get_op());

  unique_ptr<Expression> &predicate_expr = expressions.front();
  vector<unique_ptr<Expression>> pushdown_exprs;

  // Extract pushable expressions
  if (predicate_expr->type() == ExprType::CONJUNCTION) {
    ConjunctionExpr *conjunction_expr = static_cast<ConjunctionExpr *>(predicate_expr.get());
    if (conjunction_expr->conjunction_type() == ConjunctionExpr::Type::OR) {
      // OR operations are too complex, not supported yet
      return;
    }

    vector<unique_ptr<Expression>> &child_exprs = conjunction_expr->children();
    for (auto &child_expr : child_exprs) {
      pushdown_exprs.push_back(child_expr->copy());
    }
  } else if (predicate_expr->type() == ExprType::COMPARISON) {
    // Check if it's a constant expression
    pushdown_exprs.push_back(predicate_expr->copy());
  }

  if (pushdown_exprs.empty()) {
    return;
  }

  // Create new TableGetLogicalOperator with pushed predicates
  // Merge existing predicates with pushed predicates
  for (auto &expr : table_get_oper->predicates()) {
    pushdown_exprs.push_back(expr->copy());
  }
  
  auto new_table_get = make_unique<TableGetLogicalOperator>(
      table_get_oper->table(), table_get_oper->read_write_mode());
  new_table_get->set_predicates(std::move(pushdown_exprs));

  // Return new TableGet, replacing Filter (TableGet is a leaf node with no children)
  transformed->emplace_back(std::move(new_table_get));
}

// -------------------------------------------------------------------------------------------------
// PredicateRewriteRule
// -------------------------------------------------------------------------------------------------
PredicateRewriteRule::PredicateRewriteRule()
{
  type_          = RuleType::PREDICATE_REWRITE;
  match_pattern_ = unique_ptr<Pattern>(new Pattern(OpType::LOGICALFILTER));
  auto child     = new Pattern(OpType::LEAF);
  match_pattern_->add_child(child);
}

void PredicateRewriteRule::transform(
    GroupExpr *input, std::vector<CandidateExpression> *transformed, OptimizerContext *context) const
{
  ASSERT(input->get_children_groups_size() == 1, "Filter should have 1 child");

  auto predicate_oper = static_cast<PredicateLogicalOperator *>(input->get_op());
  vector<unique_ptr<Expression>> &expressions = predicate_oper->expressions();
  if (expressions.size() != 1) {
    return;
  }

  unique_ptr<Expression> &expr = expressions.front();
  if (expr->type() != ExprType::VALUE) {
    return;
  }

  // Check if it's constant true or false
  auto value_expr = static_cast<ValueExpr *>(expr.get());
  bool bool_value = value_expr->get_value().get_boolean();

  if (bool_value == true) {
    // Constant true: remove Filter, return child directly
    Memo &memo = context->get_memo();
    memo.make_alias(input->get_group_id(), input->get_child_group_ids()[0]);
  } else {
    // Constant false: return empty operator in Cascade
    transformed->emplace_back(std::unique_ptr<OperatorNode>(new EmptyLogicalOperator));
    return;
  }
}

// -------------------------------------------------------------------------------------------------
// ExpressionSimplifyRule
// -------------------------------------------------------------------------------------------------
ExpressionSimplifyRule::ExpressionSimplifyRule()
{
  type_          = RuleType::EXPRESSION_SIMPLIFY;
  match_pattern_ = unique_ptr<Pattern>(new Pattern(OpType::LOGICALFILTER));
  auto child     = new Pattern(OpType::LEAF);
  match_pattern_->add_child(child);
}

void ExpressionSimplifyRule::transform(
    GroupExpr *input, std::vector<CandidateExpression> *transformed, OptimizerContext *context) const
{
  auto predicate_oper = static_cast<PredicateLogicalOperator *>(input->get_op());
  vector<unique_ptr<Expression>> &expressions = predicate_oper->expressions();
  
  bool changed = false;
  vector<unique_ptr<Expression>> new_expressions;
  
  for (auto &expr : expressions) {
    unique_ptr<Expression> new_expr = expr->copy();
    if (simplify_expression(new_expr)) {
      changed = true;
    }
    new_expressions.push_back(std::move(new_expr));
  }

  if (changed && !new_expressions.empty()) {
    auto new_predicate = make_unique<PredicateLogicalOperator>(std::move(new_expressions.front()));
    transformed->emplace_back(std::move(new_predicate), input->get_child_group_ids());
  }
}

bool ExpressionSimplifyRule::simplify_expression(unique_ptr<Expression> &expr) const
{
  bool changed = false;

  // Simplify comparison expressions
  if (expr->type() == ExprType::COMPARISON) {
    Value value;
    ComparisonExpr *cmp_expr = static_cast<ComparisonExpr *>(expr.get());
    if (cmp_expr->try_get_value(value) == RC::SUCCESS) {
      expr = make_unique<ValueExpr>(value);
      changed = true;
    }
  }

  // Simplify conjunction expressions
  if (expr->type() == ExprType::CONJUNCTION) {
    auto conjunction_expr = static_cast<ConjunctionExpr *>(expr.get());
    vector<unique_ptr<Expression>> &child_exprs = conjunction_expr->children();

    // Simplify child expressions first
    for (auto &child_expr : child_exprs) {
      if (simplify_expression(child_expr)) {
        changed = true;
      }
    }

    // Check for removable constant expressions
    for (auto iter = child_exprs.begin(); iter != child_exprs.end();) {
      bool constant_value = false;
      if ((*iter)->type() == ExprType::VALUE && (*iter)->value_type() == AttrType::BOOLEANS) {
        auto value_expr = static_cast<ValueExpr *>(iter->get());
        constant_value = value_expr->get_value().get_boolean();

        if (conjunction_expr->conjunction_type() == ConjunctionExpr::Type::AND) {
          if (constant_value == true) {
            iter = child_exprs.erase(iter);
            changed = true;
            continue;
          } else {
            // always be false
            expr = make_unique<ValueExpr>(Value((bool)false));
            changed = true;
            return changed;
          }
        } else {
          // OR
          if (constant_value == true) {
            // always be true
            expr = make_unique<ValueExpr>(Value((bool)true));
            changed = true;
            return changed;
          } else {
            iter = child_exprs.erase(iter);
            changed = true;
            continue;
          }
        }
      }
      ++iter;
    }

    // If only one child expression, replace directly
    if (child_exprs.size() == 1) {
      expr = std::move(child_exprs.front());
      changed = true;
    }
  }

  return changed;
}

// -------------------------------------------------------------------------------------------------
// JoinCommutativityRule
// -------------------------------------------------------------------------------------------------
JoinCommutativityRule::JoinCommutativityRule()
{
  type_          = RuleType::JOIN_COMMUTATIVITY;
  match_pattern_ = unique_ptr<Pattern>(new Pattern(OpType::LOGICALINNERJOIN));
  auto left      = new Pattern(OpType::LEAF);
  auto right     = new Pattern(OpType::LEAF);
  match_pattern_->add_child(left);
  match_pattern_->add_child(right);
}

void JoinCommutativityRule::transform(
    GroupExpr *input, std::vector<CandidateExpression> *transformed, OptimizerContext *context) const
{
  (void)context;
  ASSERT(input->get_children_groups_size() == 2, "join should have 2 children");
  if (input->get_op()->get_op_type() != OpType::LOGICALINNERJOIN) {
    return;
  }

  auto join_oper = static_cast<JoinLogicalOperator *>(input->get_op());
  auto new_join  = join_oper->clone();

  const auto &child_ids = input->get_child_group_ids();
  transformed->emplace_back(std::move(new_join), std::vector<int>{child_ids[1], child_ids[0]});
}

// -------------------------------------------------------------------------------------------------
// JoinAssociativityRule
// -------------------------------------------------------------------------------------------------
JoinAssociativityRule::JoinAssociativityRule()
{
  type_          = RuleType::JOIN_ASSOCIATIVITY;
  match_pattern_ = unique_ptr<Pattern>(new Pattern(OpType::LOGICALINNERJOIN));

  // Match: Join( Join(A, B), C )
  auto left_join = new Pattern(OpType::LOGICALINNERJOIN);
  left_join->add_child(new Pattern(OpType::LEAF));
  left_join->add_child(new Pattern(OpType::LEAF));
  auto right = new Pattern(OpType::LEAF);

  match_pattern_->add_child(left_join);
  match_pattern_->add_child(right);
}

void JoinAssociativityRule::transform(
    GroupExpr *input, std::vector<CandidateExpression> *transformed, OptimizerContext *context) const
{
  ASSERT(input->get_children_groups_size() == 2, "join should have 2 children");
  if (input->get_op()->get_op_type() != OpType::LOGICALINNERJOIN) {
    return;
  }

  // Current shape: Join( left_group, c_group )
  const int left_group_id = input->get_child_group_id(0);
  const int c_group_id    = input->get_child_group_id(1);

  Memo  &memo       = context->get_memo();
  Group *left_group = memo.get_group_by_id(left_group_id);
  if (left_group == nullptr) {
    return;
  }

  // The left child is a GROUP, which may contain multiple logical expressions after
  // commutativity (and other) transformations. Try each Join(A,B) logical expression.
  const auto &left_logical_exprs = left_group->get_logical_expressions();
  for (GroupExpr *left_gexpr : left_logical_exprs) {
    if (left_gexpr == nullptr || left_gexpr->get_op()->get_op_type() != OpType::LOGICALINNERJOIN) {
      continue;
    }
    if (left_gexpr->get_children_groups_size() != 2) {
      continue;
    }

    // left_gexpr shape: Join(A, B)
    const int a_group_id = left_gexpr->get_child_group_id(0);
    const int b_group_id = left_gexpr->get_child_group_id(1);

    // Build inner join: Join(B, C) with empty predicates (safe, predicates can stay on top join)
    auto inner_join = make_unique<JoinLogicalOperator>();
    CandidateExpression inner_candidate(std::move(inner_join), std::vector<int>{b_group_id, c_group_id});

    GroupExpr *inner_gexpr = nullptr;
    context->record_node_into_group(inner_candidate, &inner_gexpr);
    if (inner_gexpr == nullptr) {
      continue;
    }
    const int bc_group_id = inner_gexpr->get_group_id();

    // Build new top join: Join(A, (B join C))
    auto top_join = static_cast<JoinLogicalOperator *>(input->get_op())->clone();
    auto *top_ptr = static_cast<JoinLogicalOperator *>(top_join.get());

    // Move predicates from (A join B) up to the new top join to preserve semantics.
    auto *ab_join = static_cast<JoinLogicalOperator *>(left_gexpr->get_op());
    for (auto &pred : ab_join->get_join_predicates()) {
      top_ptr->add_join_predicate(pred->copy());
    }

    transformed->emplace_back(std::move(top_join), std::vector<int>{a_group_id, bc_group_id});
  }
}

