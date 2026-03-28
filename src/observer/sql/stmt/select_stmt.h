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
// Created by Wangyunlai on 2022/6/5.
//

#pragma once

#include "common/sys/rc.h"
#include "sql/stmt/stmt.h"
#include "storage/field/field.h"

class FieldMeta;
class FilterStmt;
class Db;
class Table;
class ConjunctionExpr;
/**
 * @brief 表示select语句
 * @ingroup Statement
 */
class SelectStmt : public Stmt
{
public:
  friend class LogicalPlanGenerator;
  SelectStmt() = default;
  ~SelectStmt() override;

  StmtType type() const override { return StmtType::SELECT; }

public:
  static RC create(Db *db, SelectSqlNode &select_sql, Stmt *&stmt);

public:
  const vector<Table *> &tables() const { return tables_; }
  FilterStmt            *filter_stmt() const { return filter_stmt_; }

  vector<unique_ptr<Expression>> &query_expressions() { return query_expressions_; }
  vector<unique_ptr<Expression>> &group_by() { return group_by_; }
  vector<unique_ptr<Expression>> &having() { return having_; }
  vector<unique_ptr<Expression>> &order_by_expressions() { return order_by_expressions_; }
  const vector<bool>             &order_by_asc() const { return order_by_asc_; }
  int                             limit() const { return limit_; }

private:
  vector<unique_ptr<Expression>> query_expressions_;
  vector<Table *>                tables_;
  std::vector<std::unique_ptr<ConjunctionExpr>> join_expres_;
  std::vector<unique_ptr<Expression>>           conditions_;
  FilterStmt                    *filter_stmt_ = nullptr;
  vector<unique_ptr<Expression>> group_by_;
  vector<unique_ptr<Expression>> having_;
  vector<unique_ptr<Expression>> order_by_expressions_;
  vector<bool>                   order_by_asc_;
  int                            limit_ = -1;
};
