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
// Created by Wangyunlai on 2022/5/22.
//

#include "sql/stmt/filter_stmt.h"
#include "common/lang/string.h"
#include "common/log/log.h"
#include "common/sys/rc.h"
#include "common/type/data_type.h"
#include "storage/db/db.h"
#include "storage/table/table.h"

RC get_table_and_field(Db *db, Table *default_table, unordered_map<string, Table *> *tables,
  const RelAttrSqlNode &attr, Table *&table, const FieldMeta *&field);

RC FilterStmt::create(Db *db, Table *default_table, unordered_map<string, Table *> *tables,
    const ConditionSqlNode *conditions, int condition_num, FilterStmt *&stmt)
{
  stmt = nullptr;
  if (condition_num == 0) {
    return RC::SUCCESS;
  }

  auto implicit_cast_cost = [](AttrType from, AttrType to) -> int {
    if (from == to) {
      return 0;
    }
    return DataType::type_instance(from)->cast_cost(to);
  };

  RC rc = RC::SUCCESS;
  unique_ptr<FilterStmt> tmp_stmt(new FilterStmt());
  vector<unique_ptr<Expression>> cmp_exprs;
  cmp_exprs.reserve(condition_num);

  for (int i = 0; i < condition_num; i++) {
    const ConditionSqlNode &condition = conditions[i];
    CompOp                  comp      = condition.comp;
    if (comp < EQUAL_TO || comp >= NO_OP) {
      LOG_WARN("invalid compare operator : %d", comp);
      return RC::INVALID_ARGUMENT;
    }

    unique_ptr<Expression> left;
    if (condition.left_is_attr) {
      Table           *table = nullptr;
      const FieldMeta *field = nullptr;
      rc                     = get_table_and_field(db, default_table, tables, condition.left_attr, table, field);
      if (rc != RC::SUCCESS) {
        LOG_WARN("cannot find attr");
        return rc;
      }
      left.reset(new FieldExpr(Field(table, field)));
    } else {
      left.reset(new ValueExpr(condition.left_value));
    }

    unique_ptr<Expression> right;
    if (condition.right_is_attr) {
      Table           *table = nullptr;
      const FieldMeta *field = nullptr;
      rc                     = get_table_and_field(db, default_table, tables, condition.right_attr, table, field);
      if (rc != RC::SUCCESS) {
        LOG_WARN("cannot find attr");
        return rc;
      }
      right.reset(new FieldExpr(Field(table, field)));
    } else {
      right.reset(new ValueExpr(condition.right_value));
    }

    if (left->value_type() != right->value_type()) {
      auto left_to_right_cost = implicit_cast_cost(left->value_type(), right->value_type());
      auto right_to_left_cost = implicit_cast_cost(right->value_type(), left->value_type());
      if (left_to_right_cost <= right_to_left_cost && left_to_right_cost != INT32_MAX) {
        ExprType left_type = left->type();
        auto     cast_expr = make_unique<CastExpr>(std::move(left), right->value_type());
        if (left_type == ExprType::VALUE) {
          Value left_val;
          if (OB_FAIL(rc = cast_expr->try_get_value(left_val))) {
            LOG_WARN("failed to get value from left child", strrc(rc));
            return rc;
          }
          left = make_unique<ValueExpr>(left_val);
        } else {
          left = std::move(cast_expr);
        }
      } else if (right_to_left_cost < left_to_right_cost && right_to_left_cost != INT32_MAX) {
        ExprType right_type = right->type();
        auto     cast_expr  = make_unique<CastExpr>(std::move(right), left->value_type());
        if (right_type == ExprType::VALUE) {
          Value right_val;
          if (OB_FAIL(rc = cast_expr->try_get_value(right_val))) {
            LOG_WARN("failed to get value from right child", strrc(rc));
            return rc;
          }
          right = make_unique<ValueExpr>(right_val);
        } else {
          right = std::move(cast_expr);
        }

      } else {
        rc = RC::UNSUPPORTED;
        LOG_WARN("unsupported cast from %s to %s", attr_type_to_string(left->value_type()),
            attr_type_to_string(right->value_type()));
        return rc;
      }
    }

    cmp_exprs.emplace_back(new ComparisonExpr(comp, std::move(left), std::move(right)));
  }

  if (cmp_exprs.size() == 1) {
    tmp_stmt->predicate_ = std::move(cmp_exprs[0]);
  } else if (!cmp_exprs.empty()) {
    tmp_stmt->predicate_.reset(new ConjunctionExpr(ConjunctionExpr::Type::AND, cmp_exprs));
  }

  stmt = tmp_stmt.release();
  return RC::SUCCESS;
}

RC FilterStmt::create(unique_ptr<Expression> predicate, FilterStmt *&stmt)
{
  stmt = nullptr;
  if (predicate == nullptr) {
    return RC::SUCCESS;
  }

  FilterStmt *tmp_stmt = new FilterStmt();
  tmp_stmt->predicate_ = std::move(predicate);
  stmt                = tmp_stmt;
  return RC::SUCCESS;
}

RC get_table_and_field(Db *db, Table *default_table, unordered_map<string, Table *> *tables,
    const RelAttrSqlNode &attr, Table *&table, const FieldMeta *&field)
{
  if (common::is_blank(attr.relation_name.c_str())) {
    table = default_table;
  } else if (nullptr != tables) {
    auto iter = tables->find(attr.relation_name);
    if (iter != tables->end()) {
      table = iter->second;
    }
  } else {
    table = db->find_table(attr.relation_name.c_str());
  }
  if (nullptr == table) {
    LOG_WARN("No such table: attr.relation_name: %s", attr.relation_name.c_str());
    return RC::SCHEMA_TABLE_NOT_EXIST;
  }

  field = table->table_meta().field(attr.attribute_name.c_str());
  if (nullptr == field) {
    LOG_WARN("no such field in table: table %s, field %s", table->name(), attr.attribute_name.c_str());
    table = nullptr;
    return RC::SCHEMA_FIELD_NOT_EXIST;
  }

  return RC::SUCCESS;
}
