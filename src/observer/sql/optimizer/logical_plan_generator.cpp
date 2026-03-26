/* Copyright (c) 2023 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

//
// Created by Wangyunlai on 2023/08/16.
//

#include "sql/optimizer/logical_plan_generator.h"

#include "common/log/log.h"

#include "sql/operator/logical/calc_logical_operator.h"
#include "sql/operator/logical/delete_logical_operator.h"
#include "sql/operator/logical/explain_logical_operator.h"
#include "sql/operator/logical/insert_logical_operator.h"
#include "sql/operator/logical/join_logical_operator.h"
#include "sql/operator/logical_operator.h"
#include "sql/operator/logical/predicate_logical_operator.h"
#include "sql/operator/logical/project_logical_operator.h"
#include "sql/operator/logical/table_get_logical_operator.h"
#include "sql/operator/logical/group_by_logical_operator.h"

#include "sql/stmt/calc_stmt.h"
#include "sql/stmt/delete_stmt.h"
#include "sql/stmt/explain_stmt.h"
#include "sql/stmt/filter_stmt.h"
#include "sql/stmt/insert_stmt.h"
#include "sql/stmt/select_stmt.h"
#include "sql/stmt/stmt.h"

#include "sql/expr/expression_iterator.h"
#include "sql/optimizer/cascade/optimizer_context.h"
#include "sql/optimizer/cascade/group_expr.h"
#include "sql/optimizer/cascade/memo.h"

using namespace std;
using namespace common;

RC LogicalPlanGenerator::create(Stmt *stmt, GroupExpr *&root_gexpr, OptimizerContext *context)
{
  RC rc = RC::SUCCESS;
  switch (stmt->type()) {
    case StmtType::CALC: {
      CalcStmt *calc_stmt = static_cast<CalcStmt *>(stmt);
      rc = create_plan(calc_stmt, root_gexpr, context);
    } break;

    case StmtType::SELECT: {
      SelectStmt *select_stmt = static_cast<SelectStmt *>(stmt);
      rc = create_plan(select_stmt, root_gexpr, context);
    } break;

    case StmtType::INSERT: {
      InsertStmt *insert_stmt = static_cast<InsertStmt *>(stmt);
      rc = create_plan(insert_stmt, root_gexpr, context);
    } break;

    case StmtType::DELETE: {
      DeleteStmt *delete_stmt = static_cast<DeleteStmt *>(stmt);
      rc = create_plan(delete_stmt, root_gexpr, context);
    } break;

    case StmtType::EXPLAIN: {
      ExplainStmt *explain_stmt = static_cast<ExplainStmt *>(stmt);
      rc = create_plan(explain_stmt, root_gexpr, context);
    } break;
    default: {
      rc = RC::UNIMPLEMENTED;
    }
  }
  return rc;
}

RC LogicalPlanGenerator::create_plan(CalcStmt *calc_stmt, GroupExpr *&root_gexpr, OptimizerContext *context)
{
  unique_ptr<OperatorNode> calc_op(new CalcLogicalOperator(std::move(calc_stmt->expressions())));
  context->record_node_into_group(std::move(calc_op), &root_gexpr);
  return RC::SUCCESS;
}

RC LogicalPlanGenerator::create_plan(SelectStmt *select_stmt, GroupExpr *&root_gexpr, OptimizerContext *context)
{
  GroupExpr *last_gexpr = nullptr;
  // GroupExpr *group_by_gexpr = nullptr;

  // 1. 创建table get和join
  const vector<Table *> &tables = select_stmt->tables();
  size_t table_idx = 0;
  for (Table *table : tables) {
    unique_ptr<OperatorNode> table_get_op(new TableGetLogicalOperator(table, ReadWriteMode::READ_ONLY));
    GroupExpr *table_get_gexpr = nullptr;
    context->record_node_into_group(std::move(table_get_op), &table_get_gexpr);

    if (last_gexpr == nullptr) {
      last_gexpr = table_get_gexpr;
    } else {
      // 创建join
      unique_ptr<OperatorNode> join_op(new JoinLogicalOperator);
      if (select_stmt->join_expres_.size() > table_idx) {
        auto &join_expr = select_stmt->join_expres_[table_idx];
        if (join_expr) {
          static_cast<JoinLogicalOperator *>(join_op.get())->add_join_predicate(std::move(join_expr));
        }
      }
      std::vector<int> child_groups = {last_gexpr->get_group_id(), table_get_gexpr->get_group_id()};
      CandidateExpression candidate(std::move(join_op), std::move(child_groups));
      context->record_node_into_group(candidate, &last_gexpr);
    }
  }

  // 2. 创建filter/predicate
  // if (select_stmt->filter_stmt()) {
  //   // legacy path
  //   RC rc = create_plan(select_stmt->filter_stmt(), last_gexpr, context, last_gexpr->get_group_id());
  //   if (OB_FAIL(rc)) {
  //     LOG_WARN("failed to create predicate logical plan. rc=%s", strrc(rc));
  //     return rc;
  //   }
  // }
  if (!select_stmt->conditions_.empty()) {
    // New path: build predicate from SelectStmt::conditions_ (already bound expressions)
    unique_ptr<Expression> predicate;
    if (select_stmt->conditions_.size() == 1) {
      predicate = std::move(select_stmt->conditions_[0]);
      select_stmt->conditions_.clear();
    } else {
      vector<unique_ptr<Expression>> conjuncts;
      conjuncts.swap(select_stmt->conditions_);
      predicate = make_unique<ConjunctionExpr>(ConjunctionExpr::Type::AND, conjuncts);
    }

    unique_ptr<OperatorNode> predicate_oper(new PredicateLogicalOperator(std::move(predicate)));
    CandidateExpression      candidate(std::move(predicate_oper), {last_gexpr->get_group_id()});
    context->record_node_into_group(candidate, &last_gexpr);
  }

  // 3. 创建group by（检查是否有group by或聚合函数）
  RC rc = create_group_by_plan(select_stmt, last_gexpr, context, last_gexpr->get_group_id());
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to create group by logical plan. rc=%s", strrc(rc));
    return rc;
  }

  // 4. 创建projection
  unique_ptr<OperatorNode> project_op(new ProjectLogicalOperator(std::move(select_stmt->query_expressions())));
  CandidateExpression candidate(std::move(project_op), {last_gexpr->get_group_id()});
  context->record_node_into_group(candidate, &root_gexpr);
  return RC::SUCCESS;
}

RC LogicalPlanGenerator::create_plan(FilterStmt *filter_stmt, GroupExpr *&root_gexpr, OptimizerContext *context, int gid)
{
  RC rc = RC::SUCCESS;

  Expression *predicate = filter_stmt->predicate();
  if (predicate == nullptr) {
    return RC::SUCCESS;
  }

  unique_ptr<OperatorNode> predicate_oper(new PredicateLogicalOperator(predicate->copy()));
  CandidateExpression candidate(std::move(predicate_oper), {gid});
  context->record_node_into_group(candidate, &root_gexpr);
  return rc;
}

RC LogicalPlanGenerator::create_plan(InsertStmt *insert_stmt, GroupExpr *&root_gexpr, OptimizerContext *context)
{
  Table        *table = insert_stmt->table();
  vector<Value> values(insert_stmt->values(), insert_stmt->values() + insert_stmt->value_amount());
  unique_ptr<OperatorNode> insert_op(new InsertLogicalOperator(table, values));
  context->record_node_into_group(std::move(insert_op), &root_gexpr);
  return RC::SUCCESS;
}

RC LogicalPlanGenerator::create_plan(DeleteStmt *delete_stmt, GroupExpr *&root_gexpr, OptimizerContext *context)
{
  Table *table = delete_stmt->table();
  FilterStmt *filter_stmt = delete_stmt->filter_stmt();

  // 1. 创建table get
  unique_ptr<OperatorNode> table_get_op(new TableGetLogicalOperator(table, ReadWriteMode::READ_WRITE));
  GroupExpr *table_get_gexpr = nullptr;
  context->record_node_into_group(std::move(table_get_op), &table_get_gexpr);

  // 2. 创建predicate（如果有）
  GroupExpr *predicate_gexpr = nullptr;
  GroupExpr *last_gexpr = table_get_gexpr;
  
  if (filter_stmt) {
    RC rc = create_plan(filter_stmt, predicate_gexpr, context, last_gexpr->get_group_id());
    if (OB_FAIL(rc)) {
      return rc;
    }
    last_gexpr = predicate_gexpr;
  }

  // 3. 创建delete
  unique_ptr<OperatorNode> delete_op(new DeleteLogicalOperator(table));
  std::vector<int> child_groups = {last_gexpr->get_group_id()};
  CandidateExpression candidate(std::move(delete_op), std::move(child_groups));
  context->record_node_into_group(candidate, &root_gexpr);

  return RC::SUCCESS;
}

RC LogicalPlanGenerator::create_plan(ExplainStmt *explain_stmt, GroupExpr *&root_gexpr, OptimizerContext *context)
{
  Stmt *child_stmt = explain_stmt->child();
  GroupExpr *child_gexpr = nullptr;

  RC rc = create(child_stmt, child_gexpr, context);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to create explain's child operator. rc=%s", strrc(rc));
    return rc;
  }

  unique_ptr<OperatorNode> explain_op(new ExplainLogicalOperator);
  if (child_gexpr) {
    std::vector<int> child_groups = {child_gexpr->get_group_id()};
    CandidateExpression candidate(std::move(explain_op), std::move(child_groups));
    bool inserted = context->record_node_into_group(candidate, &root_gexpr);
    if (!inserted) {
      Memo &memo = context->get_memo();
      auto group = memo.get_group_by_id(root_gexpr->get_group_id());
      root_gexpr = group->get_logical_expression();
    }
  } else {
    context->record_node_into_group(std::move(explain_op), &root_gexpr);
  }

  return rc;
}

RC LogicalPlanGenerator::create_group_by_plan(SelectStmt *select_stmt, GroupExpr *&root_gexpr, OptimizerContext *context, int gid)
{
  vector<unique_ptr<Expression>> &group_by_expressions = select_stmt->group_by();
  vector<Expression *> aggregate_expressions;
  vector<unique_ptr<Expression>> &query_expressions = select_stmt->query_expressions();
  function<RC(unique_ptr<Expression>&)> collector = [&](unique_ptr<Expression> &expr) -> RC {
    RC rc = RC::SUCCESS;
    if (expr->type() == ExprType::AGGREGATION) {
      expr->set_pos(aggregate_expressions.size() + group_by_expressions.size());
      aggregate_expressions.push_back(expr.get());
    }
    rc = ExpressionIterator::iterate_child_expr(*expr, collector);
    return rc;
  };

  function<RC(unique_ptr<Expression>&)> bind_group_by_expr = [&](unique_ptr<Expression> &expr) -> RC {
    RC rc = RC::SUCCESS;
    for (size_t i = 0; i < group_by_expressions.size(); i++) {
      auto &group_by = group_by_expressions[i];
      if (expr->type() == ExprType::AGGREGATION) {
        break;
      } else if (expr->equal(*group_by)) {
        expr->set_pos(i);
        continue;
      } else {
        rc = ExpressionIterator::iterate_child_expr(*expr, bind_group_by_expr);
      }
    }
    return rc;
  };

 bool found_unbound_column = false;
  function<RC(unique_ptr<Expression>&)> find_unbound_column = [&](unique_ptr<Expression> &expr) -> RC {
    RC rc = RC::SUCCESS;
    if (expr->type() == ExprType::AGGREGATION) {
      // do nothing
    } else if (expr->pos() != -1) {
      // do nothing
    } else if (expr->type() == ExprType::FIELD) {
      found_unbound_column = true;
    }else {
      rc = ExpressionIterator::iterate_child_expr(*expr, find_unbound_column);
    }
    return rc;
  };


  for (unique_ptr<Expression> &expression : query_expressions) {
    bind_group_by_expr(expression);
  }

  for (unique_ptr<Expression> &expression : query_expressions) {
    find_unbound_column(expression);
  }

  // collect all aggregate expressions
  for (unique_ptr<Expression> &expression : query_expressions) {
    collector(expression);
  }

  if (group_by_expressions.empty() && aggregate_expressions.empty()) {
    // 既没有group by也没有聚合函数，不需要group by
    return RC::SUCCESS;
  }

  if (found_unbound_column) {
    LOG_WARN("column must appear in the GROUP BY clause or must be part of an aggregate function");
    return RC::INVALID_ARGUMENT;
  }

  // 如果只需要聚合，但是没有group by 语句，需要生成一个空的group by 语句
  auto group_by_oper = std::unique_ptr<OperatorNode>(new GroupByLogicalOperator(std::move(group_by_expressions),
                                                           std::move(aggregate_expressions)));
  std::vector<int> child_groups = {gid};
  CandidateExpression candidate(std::move(group_by_oper), std::move(child_groups));
  context->record_node_into_group(candidate, &root_gexpr);
  return RC::SUCCESS;
}