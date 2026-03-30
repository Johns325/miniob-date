/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

//
// Created by Wangyunlai on 2022/6/6.
//

#include "sql/stmt/select_stmt.h"
#include "common/lang/string.h"
#include "common/log/log.h"
#include "sql/stmt/filter_stmt.h"
#include "storage/db/db.h"
#include "storage/table/table.h"
#include "sql/parser/expression_binder.h"

using namespace std;
using namespace common;

SelectStmt::~SelectStmt()
{
  if (nullptr != filter_stmt_) {
    delete filter_stmt_;
    filter_stmt_ = nullptr;
  }
}

RC check_join_validation(UnboundFieldExpr *expr, BinderContext &info, size_t index)
{
  Table *table{nullptr};
  auto  &tables = info.query_tables();
  if (!is_blank(expr->table_name())) {
    // table name不为空有两种情况，第一种是直接给的表名，第二中情况下是别名
    if (0 == strcmp(expr->table_name(), tables[index]->name())) {
      table      = tables[index];
      auto field = table->table_meta().field(expr->field_name());
      if (field != nullptr) {
        return RC::SUCCESS;
      }
    } else {
      for (auto k = static_cast<int>(index - 1); k >= 0; --k) {
        table = nullptr;
        if (0 == strcmp(expr->table_name(), tables[k]->name())) {
          table = tables[k];
          if (table->table_meta().field(expr->field_name()) != nullptr) {
            return RC::SUCCESS;
          }
        }
      }
    }

    return RC::SCHEMA_FIELD_NOT_EXIST;
  }
  // only specify the field_name;
  const FieldMeta *meta{nullptr};
  for (int k = static_cast<int>(index); k >= 0; --k) {
    table = tables[k];
    if (table->table_meta().field(expr->field_name())) {
      if (meta != nullptr) {
        // field is ambiguous
        return RC::SCHEMA_FIELD_AMBIGUOUS;
      }
      meta = table->table_meta().field(expr->field_name());
    }
  }
  return (meta == nullptr ? RC::SCHEMA_DB_NOT_EXIST : RC::SUCCESS);
}

/**
 * relations:     从parser 中解析到的realtions. 例如select * from a,b; 那么relations:{a,b}.
 * table_map:     name to table mapping.
 * alias2table:   alias to table mapping.
 * ctx:           used to store binding information of current query.
 * tables:        tables current query references.
 * join_exprs:    join conditions within current query.
 */
std::pair<RC, ExpressionBinder *> bind_from(Db *db, std::vector<std::unique_ptr<rel_info>> &relations,
    BinderContext &ctx, std::vector<unique_ptr<ConjunctionExpr>> &join_exprs)
{
  // auto &info = ctx.bound_info();
  /* 维护当前(子)查询的表名到表和别名到表的映射。*/
  for (size_t i = 0; i < relations.size(); i++) {

    auto table_name = (*relations[i]).relation_name;
    if (table_name.empty()) {
      LOG_WARN("invalid argument. relation name is empty. index=%d", i);
      return {RC::INVALID_ARGUMENT, nullptr};
    }

    Table *table = db->find_table(table_name.c_str());
    if (nullptr == table) {
      LOG_WARN("no such table. db=%s, table_name=%s", db->name(), table_name.c_str());
      return {RC::SCHEMA_TABLE_NOT_EXIST, nullptr};
    }

    ASSERT(string(table->name()) == table_name, "These two names must be equal");
    ctx.add_table(table);
  }

  if (!(*relations[0]).on_conditions.empty()) {
    // bind join condtions.
    // almost unreachable.
    return {RC::INTERNAL, nullptr};
  }

  auto binder = new ExpressionBinder(ctx);
  // tabe[i].on_conditions refers table_i and the joined table which is the join table of tables before table_i.
  // 应该先对on_conditions中的表达式进行绑定后再判定有效性
  // std::vector<unique_ptr<ComparisonExpr>> join_exprs;
  for (size_t k = 1; k < relations.size(); ++k) {
    auto &rel_info = *relations[k];
    if (!rel_info.on_conditions.empty()) {
      std::vector<unique_ptr<Expression>> bound_expres;
      for (auto &expr : rel_info.on_conditions) {
        // cmp shall be a ComparisonExpr.
        // 现在假设on 中的条件只包含ValueExpr 和 FieldExpr.
        ASSERT(expr->type() == ExprType::COMPARISON, "Expression type is invalid");
        auto cmp = static_cast<ComparisonExpr *>(expr.get());
        if (cmp->left()->type() == ExprType::UNBOUND_FIELD) {
          auto unbound_expr = static_cast<UnboundFieldExpr *>(cmp->left().get());
          auto rc           = check_join_validation(unbound_expr, ctx, k);
          if (!OB_SUCC(rc)) {
            delete binder;
            return {rc, nullptr};
          }
        }

        if (cmp->right()->type() == ExprType::UNBOUND_FIELD) {
          auto unbound_expr = static_cast<UnboundFieldExpr *>(cmp->right().get());
          auto rc           = check_join_validation(unbound_expr, ctx, k);
          if (!OB_SUCC(rc)) {
            delete binder;
            return {rc, nullptr};
          }
        }
        // std::unique_ptr<Expression> expr1(cmp);
        auto rc = binder->bind_expression(expr, bound_expres);
        if (OB_FAIL(rc)) {
          LOG_INFO("bind expression failed. rc=%s", strrc(rc));
          delete binder;
          return {rc, nullptr};
        }
      }
      // 再做一个ConjunctionExpression
      auto ptr = std::unique_ptr<ConjunctionExpr>(new ConjunctionExpr(ConjunctionExpr::Type::AND, bound_expres));
      join_exprs.emplace_back(std::move(ptr));
    } else {
      join_exprs.emplace_back(nullptr);
    }
  }
  return {RC::SUCCESS, binder};
}

RC bind_where(Db *db, ExpressionBinder *binder, std::vector<std::unique_ptr<Expression>> &expressions,
    vector<unique_ptr<Expression>> &bound_expressions)
{
  if (0 == expressions.size()) {
    return RC::SUCCESS;
  }
  for (auto &expression : expressions) {
    RC rc = binder->bind_expression(expression, bound_expressions);
    if (OB_FAIL(rc)) {
      LOG_INFO("bind expression failed. rc=%s", strrc(rc));
      return rc;
    }
  }
  return RC::SUCCESS;
}

RC SelectStmt::create(Db *db, SelectSqlNode &select_sql, Stmt *&stmt)
{
  if (nullptr == db) {
    LOG_WARN("invalid argument. db is null");
    return RC::INVALID_ARGUMENT;
  }

  BinderContext binder_context;

  // step 1 collect tables in `from` statement
  BinderContext                                 context;
  vector<Table *>                               tables;
  unordered_map<string, Table *>                table_map;
  std::vector<std::unique_ptr<ConjunctionExpr>> join_expres;
  std::unique_ptr<ExpressionBinder>             expression_binder;
  /* ******************************************************{binding from}*********************************************************************/
  {
    // step1 开始绑定FROM
    auto res = bind_from(db, select_sql.relations, context, join_expres);
    if (!OB_SUCC(res.first)) {
      return res.first;
    }
    expression_binder.reset(res.second);
  }

  // step 2 collect query fields in `select` statement
  vector<unique_ptr<Expression>> bound_expressions;

  for (unique_ptr<Expression> &expression : select_sql.expressions) {
    RC rc = expression_binder->bind_expression(expression, bound_expressions);
    if (OB_FAIL(rc)) {
      LOG_INFO("bind expression failed. rc=%s", strrc(rc));
      return rc;
    }
  }

   /* ******************************************************{binding where}*******************************************************************/
  vector<unique_ptr<Expression>> bound_where_expressions;  // 可以直接丢给Predicate Operator
  {
    auto rc = bind_where(db, expression_binder.get(), select_sql.where, bound_where_expressions);
    if (!OB_SUCC(rc)) {
      return rc;
    }
  } // end of scope

  /* ******************************************************{binding group by}*****************************************************************/
  vector<unique_ptr<Expression>> bound_group_by_expressions;
  {
    auto rc = bind_where(db, expression_binder.get(), select_sql.group_by, bound_group_by_expressions);
    if (!OB_SUCC(rc)) {
      return rc;
    }
  }

  /* ******************************************************{binding having}*******************************************************************/
  vector<unique_ptr<Expression>> bound_having_expressions;
  {
    auto rc = bind_where(db, expression_binder.get(), select_sql.having, bound_having_expressions);
    if (!OB_SUCC(rc)) {
      return rc;
    }
  }

  /* ******************************************************{binding order by}*****************************************************************/
  vector<unique_ptr<Expression>> bound_order_by_expressions;
  vector<bool>                   bound_order_by_asc;
  {
    for (auto &item : select_sql.order_by) {
      unique_ptr<Expression> order_expr = std::move(item.expr);
      vector<unique_ptr<Expression>> child_bound_expressions;
      RC rc = expression_binder->bind_expression(order_expr, child_bound_expressions);
      if (OB_FAIL(rc)) {
        LOG_INFO("bind expression failed. rc=%s", strrc(rc));
        return rc;
      }

      if (child_bound_expressions.size() != 1) {
        LOG_INFO("invalid children number of order by expression: %d", child_bound_expressions.size());
        return RC::INVALID_ARGUMENT;
      }

      if (child_bound_expressions[0].get() != order_expr.get()) {
        order_expr.reset(child_bound_expressions[0].release());
      }

      bound_order_by_expressions.emplace_back(std::move(order_expr));
      bound_order_by_asc.emplace_back(item.asc);
    }
  }

  // step 4 everything alright
  SelectStmt *select_stmt = new SelectStmt();
  auto referenced_tables = context.query_tables();
  select_stmt->tables_.swap(referenced_tables);
  select_stmt->join_expres_.swap(join_expres);
  select_stmt->query_expressions_.swap(bound_expressions);
  select_stmt->conditions_.swap(bound_where_expressions);
  select_stmt->group_by_.swap(bound_group_by_expressions);
  select_stmt->having_.swap(bound_having_expressions);
  select_stmt->order_by_expressions_.swap(bound_order_by_expressions);
  select_stmt->order_by_asc_.swap(bound_order_by_asc);
  select_stmt->limit_ = select_sql.limit;
  stmt = select_stmt;
  return RC::SUCCESS;
}