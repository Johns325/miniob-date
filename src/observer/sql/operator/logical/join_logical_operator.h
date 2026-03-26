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
// Created by Wangyunlai on 2022/12/07
//

#pragma once

#include "sql/operator/logical_operator.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <string>
#include <utility>

#include "common/log/log.h"

/**
 * @brief 连接算子
 * @ingroup LogicalOperator
 * @details 连接算子，用于连接两个表。对应的物理算子或者实现，可能有NestedLoopJoin，HashJoin等等。
 */
class JoinLogicalOperator : public LogicalOperator
{
public:
  JoinLogicalOperator()          = default;
  virtual ~JoinLogicalOperator() = default;

  void add_predicate_op(LogicalOperator *predicate_op) { predicate_op_ = predicate_op; }
  auto predicates() -> Expression *
  {
    if (predicate_op_ != nullptr && predicate_op_->expressions().size() == 1) {
      return predicate_op_->expressions()[0].get();
    }
    return nullptr;
  }

  OpType get_op_type() const override { return OpType::LOGICALINNERJOIN; }

  virtual uint64_t hash() const override
  {
    uint64_t hash = std::hash<int>()(static_cast<int>(get_op_type()));
    hash ^= std::hash<size_t>()(join_predicates_.size());
    for (const auto &pred : join_predicates_) {
      hash ^= std::hash<int>()(static_cast<int>(pred->type()));
    }
    return hash;
  }

  virtual bool operator==(const OperatorNode &other) const override
  {
    if (get_op_type() != other.get_op_type())
      return false;
    const auto &other_join = static_cast<const JoinLogicalOperator &>(other);
    if (join_predicates_.size() != other_join.join_predicates_.size())
      return false;
    for (size_t i = 0; i < join_predicates_.size(); i++) {
      if (!join_predicates_[i]->equal(*(other_join.join_predicates_[i])))
        return false;
    }
    return true;
  }

  vector<unique_ptr<Expression>> &get_join_predicates() { return join_predicates_; }

  void clear_join_predicates() { join_predicates_.clear(); }

  auto add_join_predicate(unique_ptr<Expression> &&predicate) { join_predicates_.push_back(std::move(predicate)); }

  unique_ptr<LogicalOperator> clone() const override
  {
    auto new_join = make_unique<JoinLogicalOperator>();
    // 复制 join predicates
    for (auto &pred : join_predicates_) {
      new_join->add_join_predicate(pred->copy());
    }
    return new_join;
  }

  unique_ptr<LogicalProperty> find_log_prop(const vector<LogicalProperty *> &log_props) override
  {
    if (log_props.size() != 2) {
      return nullptr;
    }

    LogicalProperty *left_log_prop  = log_props[0];
    LogicalProperty *right_log_prop = log_props[1];

    const int64_t left_card  = left_log_prop ? static_cast<int64_t>(left_log_prop->get_card()) : 0;
    const int64_t right_card = right_log_prop ? static_cast<int64_t>(right_log_prop->get_card()) : 0;
    long double   est_card   = static_cast<long double>(std::max<int64_t>(1, left_card)) *
                            static_cast<long double>(std::max<int64_t>(1, right_card));

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
      if (expr->type() == ExprType::UNBOUND_FIELD) {
        auto *u = static_cast<const UnboundFieldExpr *>(expr);
        const char *t = u->table_name();
        const char *c = u->field_name();
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

    // Collect equi-join key pairs (as column keys) from predicates (including conjunction)
    std::vector<std::pair<std::string, std::string>> equi_keys;
    std::function<void(const Expression *)> collect = [&](const Expression *expr) {
      if (expr == nullptr) {
        return;
      }
      if (expr->type() == ExprType::CONJUNCTION) {
        auto *conj = static_cast<const ConjunctionExpr *>(expr);
        for (const auto &child : conj->children()) {
          collect(child.get());
        }
        return;
      }
      if (expr->type() != ExprType::COMPARISON) {
        return;
      }
      auto *cmp = static_cast<const ComparisonExpr *>(expr);
      if (cmp->comp() != CompOp::EQUAL_TO) {
        return;
      }
      std::string lk;
      std::string rk;
      if (col_key(cmp->left().get(), lk) && col_key(cmp->right().get(), rk)) {
        equi_keys.emplace_back(std::move(lk), std::move(rk));
      }
    };
    for (const auto &predicate : join_predicates_) {
      collect(predicate.get());
    }

    // Estimate cardinality using NDV when possible:
    // |R ⋈ S| ≈ |R| * |S| / max(NDV(R.a), NDV(S.b)) for equi-join R.a = S.b
    for (const auto &kv : equi_keys) {
      const std::string &lk = kv.first;
      const std::string &rk = kv.second;

      int64_t lndv = 0;
      int64_t rndv = 0;
      bool    has_l = left_log_prop && left_log_prop->get_ndv(lk, lndv);
      bool    has_r = right_log_prop && right_log_prop->get_ndv(rk, rndv);

      if (!has_l) {
        lndv = std::max<int64_t>(1, left_card);
      }
      if (!has_r) {
        rndv = std::max<int64_t>(1, right_card);
      }

      const int64_t denom = std::max<int64_t>(1, std::max(lndv, rndv));
      est_card /= static_cast<long double>(denom);
    }

    if (est_card < 1) {
      est_card = 1;
    }
    if (est_card > static_cast<long double>(std::numeric_limits<int>::max())) {
      est_card = static_cast<long double>(std::numeric_limits<int>::max());
    }
    auto out_prop = make_unique<LogicalProperty>(static_cast<int>(est_card));

    // Propagate NDVs from children
    if (left_log_prop != nullptr) {
      out_prop->merge_ndv_from(*left_log_prop);
    }
    if (right_log_prop != nullptr) {
      out_prop->merge_ndv_from(*right_log_prop);
    }

    // For join keys, NDV after equi-join becomes min(NDV(left), NDV(right)) when both available.
    for (const auto &kv : equi_keys) {
      int64_t lndv = 0;
      int64_t rndv = 0;
      const bool has_l = left_log_prop && left_log_prop->get_ndv(kv.first, lndv);
      const bool has_r = right_log_prop && right_log_prop->get_ndv(kv.second, rndv);
      if (has_l && has_r) {
        const int64_t out_ndv = std::max<int64_t>(1, std::min(lndv, rndv));
        out_prop->set_ndv(kv.first, out_ndv);
        out_prop->set_ndv(kv.second, out_ndv);
      }
    }
    out_prop->cap_ndv_by_card();
    return out_prop;
  }

private:
  LogicalOperator                     *predicate_op_    = nullptr;
  std::vector<unique_ptr<Expression>> &join_predicates_ = expressions_;
};
