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
// Created by WangYunlai on 2024/05/30.
//

#include "common/log/log.h"
#include "sql/operator/logical/group_by_logical_operator.h"
#include "sql/expr/expression.h"
#include "sql/optimizer/cascade/property.h"

using namespace std;

GroupByLogicalOperator::GroupByLogicalOperator(vector<unique_ptr<Expression>> &&group_by_exprs,
                                               vector<Expression *> &&expressions)
{
  group_by_expressions_ = std::move(group_by_exprs);
  aggregate_expressions_ = std::move(expressions);
}

GroupByLogicalOperator::GroupByLogicalOperator(vector<unique_ptr<Expression>> &&group_by_exprs,
                                               vector<unique_ptr<Expression>> &&aggregate_exprs)
{
  group_by_expressions_        = std::move(group_by_exprs);
  owned_aggregate_expressions_ = std::move(aggregate_exprs);
  aggregate_expressions_.clear();
  aggregate_expressions_.reserve(owned_aggregate_expressions_.size());
  for (auto &expr : owned_aggregate_expressions_) {
    aggregate_expressions_.push_back(expr.get());
  }
}

unique_ptr<LogicalOperator> GroupByLogicalOperator::clone() const
{
  vector<unique_ptr<Expression>> group_by_exprs;
  for (auto &expr : group_by_expressions_) {
    group_by_exprs.push_back(expr->copy());
  }

  if (!owned_aggregate_expressions_.empty()) {
    vector<unique_ptr<Expression>> agg_exprs;
    agg_exprs.reserve(owned_aggregate_expressions_.size());
    for (const auto &expr : owned_aggregate_expressions_) {
      agg_exprs.push_back(expr->copy());
    }
    return make_unique<GroupByLogicalOperator>(std::move(group_by_exprs), std::move(agg_exprs));
  }

  // aggregate expressions are referenced elsewhere (e.g. projection/select list)
  vector<Expression *> agg_exprs;
  agg_exprs.reserve(aggregate_expressions_.size());
  for (auto *expr : aggregate_expressions_) {
    agg_exprs.push_back(expr);
  }
  return make_unique<GroupByLogicalOperator>(std::move(group_by_exprs), std::move(agg_exprs));
}

unique_ptr<LogicalProperty> GroupByLogicalOperator::find_log_prop(const vector<LogicalProperty *> &log_props)
{
  if (log_props.size() != 1) {
    return nullptr;
  }

  const LogicalProperty *child_prop = log_props[0];
  const int64_t child_card = child_prop ? static_cast<int64_t>(child_prop->get_card()) : 0;
  const int64_t input_card = std::max<int64_t>(1, child_card);

  // Scalar aggregation: always produces at most one row
  if (group_by_expressions_.empty()) {
    return make_unique<LogicalProperty>(1);
  }

  auto col_key = [](const Expression *expr, std::string &out) -> bool {
    out.clear();
    if (expr == nullptr) {
      return false;
    }
    if (expr->type() == ExprType::FIELD) {
      auto *f = static_cast<const FieldExpr *>(expr);
      const char *t = f->table_name();
      const char *c = f->field_name();
      if (c == nullptr || *c == '\0') {
        return false;
      }
      if (t != nullptr && *t != '\0') {
        out = std::string(t) + "." + std::string(c);
      } else {
        out = std::string(c);
      }
      return true;
    }
    return false;
  };

  // Estimate grouped cardinality using NDV of grouping keys when available.
  int64_t est = 1;
  std::unordered_map<std::string, int64_t> key_ndv;
  for (const auto &expr : group_by_expressions_) {
    std::string key;
    if (!col_key(expr.get(), key)) {
      // For non-field grouping expressions, fall back to a conservative estimate.
      est = input_card;
      break;
    }
    int64_t ndv = 0;
    if (child_prop != nullptr && child_prop->get_ndv(key, ndv)) {
      ndv = std::max<int64_t>(1, ndv);
    } else {
      ndv = input_card;
    }
    key_ndv[key] = ndv;
    if (est < input_card) {
      long double prod = static_cast<long double>(est) * static_cast<long double>(ndv);
      if (prod >= static_cast<long double>(input_card)) {
        est = input_card;
      } else {
        est = static_cast<int64_t>(prod);
      }
    }
  }

  est = std::max<int64_t>(1, std::min<int64_t>(input_card, est));
  if (est > static_cast<int64_t>(std::numeric_limits<int>::max())) {
    est = static_cast<int64_t>(std::numeric_limits<int>::max());
  }

  auto out = make_unique<LogicalProperty>(static_cast<int>(est));
  // Keep NDVs only for grouping keys
  for (const auto &kv : key_ndv) {
    out->set_ndv(kv.first, std::max<int64_t>(1, std::min<int64_t>(kv.second, est)));
  }
  out->cap_ndv_by_card();
  return out;
}
