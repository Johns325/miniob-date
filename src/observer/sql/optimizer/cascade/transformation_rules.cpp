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
#include "sql/operator/logical/limit_logical_operator.h"
#include "sql/operator/logical/order_by_logical_operator.h"
#include "sql/operator/logical/topn_logical_operator.h"
#include "sql/operator/logical/group_by_logical_operator.h"
#include "sql/expr/expression.h"
#include "common/value.h"

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

// -------------------------------------------------------------------------------------------------
// TopNRule (Limit(OrderBy(X)) -> TopN(X))
// -------------------------------------------------------------------------------------------------
TopNRule::TopNRule()
{
  type_          = RuleType::ORDER_BY_LIMIT_TO_TOPN;
  match_pattern_ = unique_ptr<Pattern>(new Pattern(OpType::LOGICALLIMIT));

  auto order_by = new Pattern(OpType::LOGICALORDERBY);
  order_by->add_child(new Pattern(OpType::LEAF));
  match_pattern_->add_child(order_by);
}

void TopNRule::transform(
    GroupExpr *input, std::vector<CandidateExpression> *transformed, OptimizerContext *context) const
{
  ASSERT(input->get_children_groups_size() == 1, "limit should have 1 child");
  if (input->get_op()->get_op_type() != OpType::LOGICALLIMIT) {
    return;
  }

  auto *limit_oper = static_cast<LimitLogicalOperator *>(input->get_op());
  const int limit  = limit_oper->limit();
  if (limit < 0) {
    return;
  }

  Memo  &memo        = context->get_memo();
  Group *order_group = memo.get_group_by_id(input->get_child_group_ids()[0]);
  if (order_group == nullptr) {
    return;
  }

  // The child group may contain multiple logical expressions; find the ORDER BY one.
  GroupExpr *order_gexpr = nullptr;
  for (GroupExpr *gexpr : order_group->get_logical_expressions()) {
    if (gexpr != nullptr && gexpr->get_op() != nullptr && gexpr->get_op()->get_op_type() == OpType::LOGICALORDERBY) {
      order_gexpr = gexpr;
      break;
    }
  }
  if (order_gexpr == nullptr) {
    return;
  }
  if (order_gexpr->get_children_groups_size() != 1) {
    return;
  }

  auto *order_oper = static_cast<OrderByLogicalOperator *>(order_gexpr->get_op());
  if (order_oper->expressions().empty()) {
    return;
  }

  vector<unique_ptr<Expression>> order_exprs;
  order_exprs.reserve(order_oper->expressions().size());
  for (const auto &expr : order_oper->expressions()) {
    order_exprs.emplace_back(expr->copy());
  }
  vector<bool> asc = order_oper->asc();

  auto topn_oper = make_unique<TopNLogicalOperator>(std::move(order_exprs), std::move(asc), limit);
  transformed->emplace_back(std::move(topn_oper), std::vector<int>{order_gexpr->get_child_group_id(0)});
}

// -------------------------------------------------------------------------------------------------
// AggregateJoinPushdownRule
// -------------------------------------------------------------------------------------------------
namespace
{
static bool get_single_table_name_from_group(Memo &memo, int group_id, std::string &table_name_out)
{
  table_name_out.clear();
  Group *g = memo.get_group_by_id(group_id);
  if (g == nullptr) {
    return false;
  }

  const auto &exprs = g->get_logical_expressions();
  for (GroupExpr *ge : exprs) {
    if (ge == nullptr || ge->get_op() == nullptr) {
      continue;
    }
    if (ge->get_op()->get_op_type() != OpType::LOGICALGET) {
      continue;
    }
    auto *get = static_cast<TableGetLogicalOperator *>(ge->get_op());
    if (get->table() == nullptr || get->table()->name() == nullptr) {
      continue;
    }
    const std::string name(get->table()->name());
    if (name.empty()) {
      continue;
    }
    if (table_name_out.empty()) {
      table_name_out = name;
    } else if (table_name_out != name) {
      return false;
    }
  }
  return !table_name_out.empty();
}

static bool expr_table_name(const Expression *expr, std::string &table_out)
{
  table_out.clear();
  if (expr == nullptr) {
    return false;
  }
  if (expr->type() == ExprType::FIELD) {
    auto *f = static_cast<const FieldExpr *>(expr);
    const char *t = f->table_name();
    if (t != nullptr && *t != '\0') {
      table_out = t;
      return true;
    }
  }
  return false;
}

static void collect_table_names_in_expr(const Expression *expr, std::unordered_set<std::string> &tables)
{
  if (expr == nullptr) {
    return;
  }

  switch (expr->type()) {
    case ExprType::FIELD: {
      std::string t;
      if (expr_table_name(expr, t)) {
        tables.insert(std::move(t));
      }
    } break;
    case ExprType::ARITHMETIC: {
      auto *a = static_cast<const ArithmeticExpr *>(expr);
      collect_table_names_in_expr(a->left().get(), tables);
      collect_table_names_in_expr(a->right().get(), tables);
    } break;
    case ExprType::COMPARISON: {
      auto *c = static_cast<const ComparisonExpr *>(expr);
      collect_table_names_in_expr(c->left().get(), tables);
      collect_table_names_in_expr(c->right().get(), tables);
    } break;
    case ExprType::CONJUNCTION: {
      auto *c = static_cast<const ConjunctionExpr *>(expr);
      for (const auto &child : c->children()) {
        collect_table_names_in_expr(child.get(), tables);
      }
    } break;
    case ExprType::CAST: {
      auto *c = static_cast<const CastExpr *>(expr);
      collect_table_names_in_expr(c->child().get(), tables);
    } break;
    case ExprType::AGGREGATION: {
      auto *a = static_cast<const AggregateExpr *>(expr);
      collect_table_names_in_expr(a->child().get(), tables);
    } break;
    default:
      break;
  }
}

struct JoinKeyPair
{
  unique_ptr<Expression> left_key;
  unique_ptr<Expression> right_key;
};

static bool all_inner_equi_join_keys(
    const std::vector<unique_ptr<Expression>> &predicates,
    const std::string                         &left_table,
    const std::string                         &right_table,
    std::vector<JoinKeyPair>                  &out_keys)
{
  out_keys.clear();

  std::function<bool(const Expression *)> collect = [&](const Expression *expr) -> bool {
    if (expr == nullptr) {
      return true;
    }
    if (expr->type() == ExprType::CONJUNCTION) {
      auto *conj = static_cast<const ConjunctionExpr *>(expr);
      // Only handle AND; OR makes keys ambiguous for now.
      if (conj->conjunction_type() != ConjunctionExpr::Type::AND) {
        return false;
      }
      for (const auto &child : conj->children()) {
        if (!collect(child.get())) {
          return false;
        }
      }
      return true;
    }
    if (expr->type() != ExprType::COMPARISON) {
      return false;
    }
    auto *cmp = static_cast<const ComparisonExpr *>(expr);
    if (cmp->comp() != CompOp::EQUAL_TO) {
      return false;
    }
    std::string lt;
    std::string rt;
    if (!expr_table_name(cmp->left().get(), lt) || !expr_table_name(cmp->right().get(), rt)) {
      return false;
    }

    // Normalize direction to (left_table key, right_table key)
    if (lt == left_table && rt == right_table) {
      out_keys.push_back({cmp->left()->copy(), cmp->right()->copy()});
      return true;
    }
    if (lt == right_table && rt == left_table) {
      out_keys.push_back({cmp->right()->copy(), cmp->left()->copy()});
      return true;
    }
    return false;
  };

  for (const auto &p : predicates) {
    if (!collect(p.get())) {
      return false;
    }
  }
  return !out_keys.empty();
}

static void dedup_exprs(vector<unique_ptr<Expression>> &exprs)
{
  vector<unique_ptr<Expression>> dedup;
  dedup.reserve(exprs.size());
  for (auto &e : exprs) {
    bool exists = false;
    for (auto &d : dedup) {
      if (e->equal(*d)) {
        exists = true;
        break;
      }
    }
    if (!exists) {
      dedup.emplace_back(std::move(e));
    }
  }
  exprs = std::move(dedup);
}

static unique_ptr<Expression> make_join_cnt_ref(const char *cnt_name)
{
  Value one;
  one.set_int(1);
  auto cnt_ref = make_unique<AggregateExpr>(AggregateExpr::Type::COUNT, make_unique<ValueExpr>(one));
  cnt_ref->set_name(cnt_name);
  return cnt_ref;
}
}  // namespace

AggregateJoinPushdownRule::AggregateJoinPushdownRule()
{
  type_          = RuleType::AGGREGATE_JOIN_PUSHDOWN;
  match_pattern_ = unique_ptr<Pattern>(new Pattern(OpType::LOGICALGROUPBY));

  auto join = new Pattern(OpType::LOGICALINNERJOIN);
  join->add_child(new Pattern(OpType::LEAF));
  join->add_child(new Pattern(OpType::LEAF));
  match_pattern_->add_child(join);
}

void AggregateJoinPushdownRule::transform(
    GroupExpr *input, std::vector<CandidateExpression> *transformed, OptimizerContext *context) const
{
  if (input == nullptr || input->get_op() == nullptr) {
    return;
  }
  if (input->get_op()->get_op_type() != OpType::LOGICALGROUPBY) {
    return;
  }
  if (input->get_children_groups_size() != 1) {
    return;
  }

  auto *top_gb = static_cast<GroupByLogicalOperator *>(input->get_op());
  // Only scalar aggregation at the top
  if (!top_gb->group_by_expressions().empty()) {
    return;
  }
  if (top_gb->aggregate_expressions().empty()) {
    return;
  }

  Memo  &memo       = context->get_memo();
  Group *join_group = memo.get_group_by_id(input->get_child_group_id(0));
  if (join_group == nullptr) {
    return;
  }

  GroupExpr *join_gexpr = nullptr;
  for (GroupExpr *ge : join_group->get_logical_expressions()) {
    if (ge != nullptr && ge->get_op() != nullptr && ge->get_op()->get_op_type() == OpType::LOGICALINNERJOIN) {
      join_gexpr = ge;
      break;
    }
  }
  if (join_gexpr == nullptr || join_gexpr->get_children_groups_size() != 2) {
    return;
  }

  const int left_gid  = join_gexpr->get_child_group_id(0);
  const int right_gid = join_gexpr->get_child_group_id(1);

  std::string left_table;
  std::string right_table;
  if (!get_single_table_name_from_group(memo, left_gid, left_table)) {
    return;
  }
  if (!get_single_table_name_from_group(memo, right_gid, right_table)) {
    return;
  }

  auto *join_op = static_cast<JoinLogicalOperator *>(join_gexpr->get_op());
  const auto &join_preds = join_op->get_join_predicates();
  std::vector<JoinKeyPair> join_keys;
  if (!all_inner_equi_join_keys(join_preds, left_table, right_table, join_keys)) {
    return;
  }

  // Decide which side contains aggregate references. We push partial agg to the other side.
  std::unordered_set<std::string> agg_tables;
  for (Expression *agg_expr : top_gb->aggregate_expressions()) {
    if (agg_expr == nullptr || agg_expr->type() != ExprType::AGGREGATION) {
      return;
    }
    auto *a = static_cast<AggregateExpr *>(agg_expr);
    collect_table_names_in_expr(a->child().get(), agg_tables);
  }

  enum class Side { LEFT, RIGHT };
  Side agg_side = Side::LEFT;
  if (agg_tables.empty()) {
    // COUNT(*) etc: pick an arbitrary side as aggregation side; pushdown to the other.
    agg_side = Side::LEFT;
  } else if (agg_tables.size() == 1 && agg_tables.count(left_table) == 1) {
    agg_side = Side::LEFT;
  } else if (agg_tables.size() == 1 && agg_tables.count(right_table) == 1) {
    agg_side = Side::RIGHT;
  } else {
    // Aggregates reference both sides or unknown tables
    return;
  }

  const Side dup_side = (agg_side == Side::LEFT ? Side::RIGHT : Side::LEFT);

  const int agg_gid = (agg_side == Side::LEFT ? left_gid : right_gid);
  const int dup_gid = (dup_side == Side::LEFT ? left_gid : right_gid);

  // Build group-by keys for the duplicated side
  vector<unique_ptr<Expression>> dup_keys;
  dup_keys.reserve(join_keys.size());
  for (auto &k : join_keys) {
    dup_keys.emplace_back(dup_side == Side::LEFT ? k.left_key->copy() : k.right_key->copy());
  }
  dedup_exprs(dup_keys);

  // Partial aggregate on duplicated side: group by join keys, compute COUNT(1) as __cbo_join_cnt
  constexpr const char *kJoinCntName = "__CBO_JOIN_CNT";
  Value one;
  one.set_int(1);
  auto cnt_agg = make_unique<AggregateExpr>(AggregateExpr::Type::COUNT, make_unique<ValueExpr>(one));
  cnt_agg->set_name(kJoinCntName);
  vector<unique_ptr<Expression>> partial_agg_exprs;
  partial_agg_exprs.emplace_back(std::move(cnt_agg));

  auto partial_gb = make_unique<GroupByLogicalOperator>(std::move(dup_keys), std::move(partial_agg_exprs));
  CandidateExpression partial_candidate(std::move(partial_gb), std::vector<int>{dup_gid});

  GroupExpr *partial_gexpr = nullptr;
  context->record_node_into_group(partial_candidate, &partial_gexpr);
  if (partial_gexpr == nullptr) {
    return;
  }
  const int partial_gid = partial_gexpr->get_group_id();

  // Build new join: Join(agg_side, partial(dup_side)) with same predicates
  auto new_join = join_op->clone();
  CandidateExpression join_candidate(
      std::move(new_join),
      std::vector<int>{(agg_side == Side::LEFT ? agg_gid : partial_gid), (agg_side == Side::LEFT ? partial_gid : agg_gid)});

  GroupExpr *new_join_gexpr = nullptr;
  context->record_node_into_group(join_candidate, &new_join_gexpr);
  if (new_join_gexpr == nullptr) {
    return;
  }
  const int new_join_gid = new_join_gexpr->get_group_id();

  // Final scalar group by:
  // - COUNT(*) -> SUM(__cbo_join_cnt)
  // - SUM(x)   -> SUM(x * __cbo_join_cnt)
  // - MIN/MAX  -> MIN/MAX(x) (duplication removed)
  vector<unique_ptr<Expression>> final_agg_exprs;
  final_agg_exprs.reserve(top_gb->aggregate_expressions().size());

  for (Expression *orig_expr : top_gb->aggregate_expressions()) {
    auto *orig_agg = static_cast<AggregateExpr *>(orig_expr);

    // For now, only rewrite COUNT(*) when its child has no table references (e.g., COUNT(*), COUNT(1)).
    if (orig_agg->aggregate_type() == AggregateExpr::Type::COUNT) {
      std::unordered_set<std::string> child_tables;
      collect_table_names_in_expr(orig_agg->child().get(), child_tables);
      if (!child_tables.empty()) {
        return;
      }
      auto cnt_ref = make_join_cnt_ref(kJoinCntName);
      auto sum_cnt = make_unique<AggregateExpr>(AggregateExpr::Type::SUM, std::move(cnt_ref));
      sum_cnt->set_name(orig_agg->name());
      final_agg_exprs.emplace_back(std::move(sum_cnt));
      continue;
    }

    if (orig_agg->aggregate_type() == AggregateExpr::Type::SUM) {
      auto cnt_ref = make_join_cnt_ref(kJoinCntName);
      auto mul = make_unique<ArithmeticExpr>(ArithmeticExpr::Type::MUL, orig_agg->child()->copy(), std::move(cnt_ref));
      auto sum_mul = make_unique<AggregateExpr>(AggregateExpr::Type::SUM, std::move(mul));
      sum_mul->set_name(orig_agg->name());
      final_agg_exprs.emplace_back(std::move(sum_mul));
      continue;
    }

    if (orig_agg->aggregate_type() == AggregateExpr::Type::MIN || orig_agg->aggregate_type() == AggregateExpr::Type::MAX) {
      auto keep = make_unique<AggregateExpr>(orig_agg->aggregate_type(), orig_agg->child()->copy());
      keep->set_name(orig_agg->name());
      final_agg_exprs.emplace_back(std::move(keep));
      continue;
    }

    // Skip AVG/MIN/MAX? MIN/MAX supported above; AVG not rewritten.
    return;
  }

  vector<unique_ptr<Expression>> empty_group_by;
  auto final_gb = make_unique<GroupByLogicalOperator>(std::move(empty_group_by), std::move(final_agg_exprs));
  transformed->emplace_back(std::move(final_gb), std::vector<int>{new_join_gid});
}

