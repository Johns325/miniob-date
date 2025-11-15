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
#include "sql/expr/expression.h"
#include "sql/stmt/stmt.h"
#include "storage/field/field.h"
#include <memory>
#include <vector>
#include <unordered_map>

class FieldMeta;
class FilterStmt;
class Db;
class Table;

// 用来记录一个select语句相应的查询信息
// 1 查询中涉及到的tables
// 2 table_name 到 Table的映射
struct Bound_Info
{
  Bound_Info(/* args */) = default;
  ~Bound_Info()          = default;
  std::vector<Table *>                tables;               // 查询访问的所以基表
  std::unordered_map<string, Table *>      name_2_tables;        // 表名到基表的映射
  std::unordered_map<string, Table *>      alias_2_tables;       // 表的别名到基表的映射
  std::unordered_map<string, Expression *> alias_2_expressions;  // 查询中别名到表达式的映射。
  std::unordered_map<string, string>       field_aliases;
};

/**
 * @brief 表示select语句
 * @ingroup Statement
 */
class SelectStmt : public Stmt
{
  friend class LogicalPlanGenerator;
public:
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
  vector<unique_ptr<Expression>> &having() { return having_expressions_; }

private:
  vector<unique_ptr<Expression>>                query_expressions_;
  vector<Table *>                               tables_;
  std::vector<std::unique_ptr<ConjunctionExpr>> join_expres_;
  std::vector<unique_ptr<Expression>>           conditions_;
  FilterStmt                                   *filter_stmt_ = nullptr;
  vector<unique_ptr<Expression>>                group_by_;
  vector<unique_ptr<Expression>>                having_expressions_;
};
