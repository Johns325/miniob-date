/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/operator/physical/hash_join_physical_operator.h"

#include <functional>
#include <limits>
#include <string_view>

#include "sql/operator/physical/table_scan_physical_operator.h"
#include "sql/optimizer/statistics/table_statistics.h"

using std::string_view;

static int64_t estimate_ndv_from_child_for_field(const PhysicalOperator *child, const FieldExpr &field)
{
    auto table_scan = dynamic_cast<const TableScanPhysicalOperator *>(child);
    if (table_scan == nullptr || table_scan->table() == nullptr) {
        return 0;
    }

    // If the FieldExpr is bound to a table name, enforce it for simple table scans.
    const char *expr_table = field.table_name();
    if (expr_table != nullptr && *expr_table != '\0') {
        const char *scan_table = table_scan->table()->name();
        if (scan_table != nullptr && *scan_table != '\0' && strcmp(expr_table, scan_table) != 0) {
            return 0;
        }
    }

    TableStatistics stats;
    if (stats.analyze(table_scan->table(), nullptr) != RC::SUCCESS) {
        return 0;
    }
    int64_t ndv = 0;
    if (stats.get_ndv(field.field_name(), ndv) != RC::SUCCESS) {
        return 0;
    }
    return ndv;
}

size_t HashJoinPhysicalOperator::estimate_output_size() const
{
    if (children_.size() != 2) {
        return 0;
    }

    const PhysicalOperator *left  = children_[0].get();
    const PhysicalOperator *right = children_[1].get();

    const int64_t t_left  = static_cast<int64_t>(left ? left->estimate_output_size() : 0);
    const int64_t t_right = static_cast<int64_t>(right ? right->estimate_output_size() : 0);
    if (t_left <= 0 || t_right <= 0) {
        return 0;
    }

    // Find one equi-join predicate: FieldExpr = FieldExpr
    const FieldExpr *left_field  = nullptr;
    const FieldExpr *right_field = nullptr;

    std::function<void(const Expression *)> find_first = [&](const Expression *expr) {
        if (expr == nullptr || left_field != nullptr) {
            return;
        }
        if (expr->type() == ExprType::CONJUNCTION) {
            auto *conj = static_cast<const ConjunctionExpr *>(expr);
            for (const auto &child : conj->children()) {
                find_first(child.get());
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
        if (!cmp->left() || !cmp->right()) {
            return;
        }
        if (cmp->left()->type() == ExprType::FIELD && cmp->right()->type() == ExprType::FIELD) {
            left_field  = static_cast<const FieldExpr *>(cmp->left().get());
            right_field = static_cast<const FieldExpr *>(cmp->right().get());
        }
    };

    for (const auto &pred : join_predicates_) {
        find_first(pred.get());
        if (left_field != nullptr) {
            break;
        }
    }

    int64_t v_left  = 0;
    int64_t v_right = 0;
    if (left_field != nullptr && right_field != nullptr) {
        // Try direct mapping first; if mismatched due to predicate ordering, swap.
        v_left  = estimate_ndv_from_child_for_field(left, *left_field);
        v_right = estimate_ndv_from_child_for_field(right, *right_field);
        if (v_left <= 0 || v_right <= 0) {
            const int64_t alt_left  = estimate_ndv_from_child_for_field(left, *right_field);
            const int64_t alt_right = estimate_ndv_from_child_for_field(right, *left_field);
            if (alt_left > 0 && alt_right > 0) {
                v_left  = alt_left;
                v_right = alt_right;
            }
        }
    }

    if (v_left <= 0) {
        v_left = t_left;
    }
    if (v_right <= 0) {
        v_right = t_right;
    }

    const int64_t denom = std::max<int64_t>(1, std::max(v_left, v_right));
    const long double est = (static_cast<long double>(t_left) * static_cast<long double>(t_right)) /
                            static_cast<long double>(denom);
    if (est <= 0) {
        return 0;
    }
    if (est > static_cast<long double>(std::numeric_limits<size_t>::max())) {
        return std::numeric_limits<size_t>::max();
    }
    return static_cast<size_t>(est);
}

double HashJoinPhysicalOperator::calculate_cost(
    LogicalProperty *prop, const vector<LogicalProperty *> &child_log_props, CostModel *cm)
{
    if (cm == nullptr) {
        return 0.0;
    }

    // Hash join: right child is build (inner), left child is probe (outer).
    int64_t outer_rows = 0;
    int64_t inner_rows = 0;
    if (child_log_props.size() >= 2) {
        if (child_log_props[0] != nullptr) {
            outer_rows = child_log_props[0]->get_card();
        }
        if (child_log_props[1] != nullptr) {
            inner_rows = child_log_props[1]->get_card();
        }
    }
    if (outer_rows <= 0 && children_.size() >= 1 && children_[0] != nullptr) {
        outer_rows = static_cast<int64_t>(children_[0]->estimate_output_size());
    }
    if (inner_rows <= 0 && children_.size() >= 2 && children_[1] != nullptr) {
        inner_rows = static_cast<int64_t>(children_[1]->estimate_output_size());
    }
    if (outer_rows < 0) {
        outer_rows = 0;
    }
    if (inner_rows < 0) {
        inner_rows = 0;
    }

    int64_t output_rows = 0;
    if (prop != nullptr) {
        output_rows = prop->get_card();
    }
    // During optimization-time costing, this physical operator may not have children wired yet,
    // so avoid using estimate_output_size() here.
    if (output_rows <= 0) {
        output_rows = std::max<int64_t>(1, outer_rows);
    }

    const double cpu_tuple_cost    = cm->cpu_tuple_cost();
    const double cpu_operator_cost = cm->cpu_op();

    const double build_cost  = static_cast<double>(inner_rows) * (cpu_tuple_cost + cpu_operator_cost);
    const double probe_cost  = static_cast<double>(outer_rows) * (cpu_tuple_cost + cpu_operator_cost);
    const double output_cost = static_cast<double>(output_rows) * cpu_tuple_cost;

    return build_cost + probe_cost + output_cost;
}

static inline size_t hash_combine(size_t seed, size_t value)
{
    // Similar to boost::hash_combine
    return seed ^ (value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
}

size_t HashJoinPhysicalOperator::JoinKeyHasher::operator()(const JoinKey &key) const noexcept
{
    size_t seed = std::hash<size_t>()(key.values.size());
    for (const Value &v : key.values) {
        seed = hash_combine(seed, std::hash<int>()(static_cast<int>(v.attr_type())));
        const char *data = v.data();
        const int   len  = v.length();
        if (data != nullptr && len > 0) {
            seed = hash_combine(seed, std::hash<string_view>()(string_view(data, static_cast<size_t>(len))));
        } else {
            seed = hash_combine(seed, 0);
        }
    }
    return seed;
}

bool HashJoinPhysicalOperator::JoinKeyEqual::operator()(const JoinKey &a, const JoinKey &b) const noexcept
{
    if (a.values.size() != b.values.size()) {
        return false;
    }
    for (size_t i = 0; i < a.values.size(); i++) {
        if (a.values[i].compare(b.values[i]) != 0) {
            return false;
        }
    }
    return true;
}

RC HashJoinPhysicalOperator::open(Trx *trx)
{
    if (children_.size() != 2) {
        LOG_WARN("hash join operator should have 2 children");
        return RC::INTERNAL;
    }

    trx_                  = trx;
    left_                 = children_[0].get();
    right_                = children_[1].get();
    left_tuple_           = nullptr;
    have_active_matches_  = false;
    key_exprs_ready_      = false;
    key_expr_pairs_.clear();
    probe_key_exprs_.clear();
    build_key_exprs_.clear();
    right_rows_.clear();
    hash_table_.clear();

    // Flatten join predicates to collect equality comparisons for hash keys.
    std::function<void(Expression *)> collect = [&](Expression *expr) {
        if (expr == nullptr) {
            return;
        }
        if (expr->type() == ExprType::CONJUNCTION) {
            auto *conj = static_cast<ConjunctionExpr *>(expr);
            for (auto &child : conj->children()) {
                collect(child.get());
            }
            return;
        }
        if (expr->type() != ExprType::COMPARISON) {
            return;
        }

        auto *cmp = static_cast<ComparisonExpr *>(expr);
        if (cmp->comp() != CompOp::EQUAL_TO) {
            return;
        }
        key_expr_pairs_.push_back(KeyExprPair{cmp->left().get(), cmp->right().get()});
    };

    for (auto &pred : join_predicates_) {
        collect(pred.get());
    }

    if (key_expr_pairs_.empty()) {
        LOG_WARN("hash join requires at least one equality predicate");
        return RC::UNIMPLEMENTED;
    }

    // Build phase: materialize right child and build hash table.
    RC rc = right_->open(trx_);
    if (rc != RC::SUCCESS) {
        LOG_WARN("failed to open right child. rc=%s", strrc(rc));
        return rc;
    }

    while ((rc = right_->next()) == RC::SUCCESS) {
        Tuple *right_tuple = right_->current_tuple();
        if (right_tuple == nullptr) {
            LOG_WARN("right child returned null tuple");
            right_->close();
            return RC::INTERNAL;
        }

        if (!key_exprs_ready_) {
            RC init_rc = init_key_exprs_with_right_tuple(*right_tuple);
            if (init_rc != RC::SUCCESS) {
                right_->close();
                return init_rc;
            }
        }

        ValueListTuple materialized;
        rc = ValueListTuple::make(*right_tuple, materialized);
        if (rc != RC::SUCCESS) {
            LOG_WARN("failed to materialize right tuple. rc=%s", strrc(rc));
            right_->close();
            return rc;
        }

        JoinKey build_key;
        rc = make_build_key(materialized, build_key);
        if (rc != RC::SUCCESS) {
            LOG_WARN("failed to build hash key from right tuple. rc=%s", strrc(rc));
            right_->close();
            return rc;
        }

        const size_t row_idx = right_rows_.size();
        right_rows_.push_back(std::move(materialized));
        hash_table_.emplace(std::move(build_key), row_idx);
    }

    if (rc != RC::RECORD_EOF) {
        LOG_WARN("failed to iterate right child. rc=%s", strrc(rc));
        right_->close();
        return rc;
    }

    rc = right_->close();
    if (rc != RC::SUCCESS) {
        LOG_WARN("failed to close right child. rc=%s", strrc(rc));
        return rc;
    }

    // Probe phase: open left child.
    rc = left_->open(trx_);
    if (rc != RC::SUCCESS) {
        LOG_WARN("failed to open left child. rc=%s", strrc(rc));
        return rc;
    }
    return RC::SUCCESS;
}

RC HashJoinPhysicalOperator::init_key_exprs_with_right_tuple(const Tuple &right_tuple)
{
    probe_key_exprs_.clear();
    build_key_exprs_.clear();

    // Determine which side of each equality predicate belongs to the build (right) tuple
    // by trying to evaluate it on a sample right tuple.
    for (const auto &pair : key_expr_pairs_) {
        Value tmp;
        RC rc_left  = pair.probe_expr->get_value(right_tuple, tmp);
        RC rc_right = pair.build_expr->get_value(right_tuple, tmp);

        const Expression *build_expr = nullptr;
        const Expression *probe_expr = nullptr;

        if (rc_right == RC::SUCCESS && rc_left != RC::SUCCESS) {
            build_expr = pair.build_expr;
            probe_expr = pair.probe_expr;
        } else if (rc_left == RC::SUCCESS && rc_right != RC::SUCCESS) {
            build_expr = pair.probe_expr;
            probe_expr = pair.build_expr;
        } else if (rc_right == RC::SUCCESS && rc_left == RC::SUCCESS) {
            // Ambiguous (both evaluatable), keep original order.
            build_expr = pair.build_expr;
            probe_expr = pair.probe_expr;
        } else {
            LOG_WARN("cannot bind join predicate to right child (both sides not evaluatable)");
            return RC::INVALID_ARGUMENT;
        }

        build_key_exprs_.push_back(build_expr);
        probe_key_exprs_.push_back(probe_expr);
    }

    key_exprs_ready_ = true;
    return RC::SUCCESS;
}

RC HashJoinPhysicalOperator::make_key(const Tuple &tuple, const vector<const Expression *> &exprs, JoinKey &key) const
{
    key.values.clear();
    key.values.reserve(exprs.size());
    for (const Expression *expr : exprs) {
        Value v;
        RC    rc = expr->get_value(tuple, v);
        if (rc != RC::SUCCESS) {
            return rc;
        }
        key.values.push_back(v);
    }
    return RC::SUCCESS;
}

RC HashJoinPhysicalOperator::make_probe_key(const Tuple &left_tuple, JoinKey &key) const
{
    return make_key(left_tuple, probe_key_exprs_, key);
}

RC HashJoinPhysicalOperator::make_build_key(const Tuple &right_tuple, JoinKey &key) const
{
    return make_key(right_tuple, build_key_exprs_, key);
}

bool HashJoinPhysicalOperator::eval_residual_predicates(const Tuple &left_tuple, const Tuple &right_tuple) const
{
    if (join_predicates_.empty()) {
        return true;
    }

    JoinedTuple tmp_joined;
    tmp_joined.set_left(const_cast<Tuple *>(&left_tuple));
    tmp_joined.set_right(const_cast<Tuple *>(&right_tuple));

    for (const auto &pred : join_predicates_) {
        Value pred_val;
        RC    rc = pred->get_value(tmp_joined, pred_val);
        if (rc != RC::SUCCESS) {
            return false;
        }
        if (pred_val.attr_type() != AttrType::BOOLEANS) {
            return false;
        }
        if (!pred_val.get_boolean()) {
            return false;
        }
    }
    return true;
}

RC HashJoinPhysicalOperator::next()
{
    if (left_ == nullptr) {
        return RC::INTERNAL;
    }

    while (true) {
        if (have_active_matches_) {
            while (match_iter_ != match_range_.second) {
                const size_t row_idx = match_iter_->second;
                ++match_iter_;

                if (row_idx >= right_rows_.size()) {
                    continue;
                }
                ValueListTuple &right_tuple = right_rows_[row_idx];

                if (!eval_residual_predicates(*left_tuple_, right_tuple)) {
                    continue;
                }

                joined_tuple_.set_left(left_tuple_);
                joined_tuple_.set_right(&right_tuple);
                return RC::SUCCESS;
            }

            have_active_matches_ = false;
        }

        RC rc = left_->next();
        if (rc != RC::SUCCESS) {
            return rc;
        }

        left_tuple_ = left_->current_tuple();
        if (left_tuple_ == nullptr) {
            return RC::INTERNAL;
        }

        JoinKey probe_key;
        rc = make_probe_key(*left_tuple_, probe_key);
        if (rc != RC::SUCCESS) {
            return rc;
        }

        match_range_ = hash_table_.equal_range(probe_key);
        match_iter_  = match_range_.first;
        have_active_matches_ = (match_iter_ != match_range_.second);
        // loop again to emit a match or fetch next left tuple
    }
}

RC HashJoinPhysicalOperator::close()
{
    RC rc_left = RC::SUCCESS;
    if (left_ != nullptr) {
        rc_left = left_->close();
        if (rc_left != RC::SUCCESS) {
            LOG_WARN("failed to close left oper. rc=%s", strrc(rc_left));
        }
    }

    // Right child was closed after build, but call close defensively.
    if (right_ != nullptr) {
        RC rc_right = right_->close();
        if (rc_right != RC::SUCCESS && rc_right != RC::RECORD_EOF) {
            LOG_WARN("failed to close right oper. rc=%s", strrc(rc_right));
        }
    }

    left_tuple_ = nullptr;
    have_active_matches_ = false;
    return rc_left;
}

Tuple *HashJoinPhysicalOperator::current_tuple()
{
    return &joined_tuple_;
}

RC HashJoinPhysicalOperator::tuple_schema(TupleSchema &schema) const
{
    if (children_.size() != 2) {
        return RC::INTERNAL;
    }

    TupleSchema left_schema;
    RC         rc = children_[0]->tuple_schema(left_schema);
    if (rc != RC::SUCCESS) {
        return rc;
    }

    TupleSchema right_schema;
    rc = children_[1]->tuple_schema(right_schema);
    if (rc != RC::SUCCESS) {
        return rc;
    }

    for (int i = 0; i < left_schema.cell_num(); i++) {
        schema.append_cell(left_schema.cell_at(i));
    }
    for (int i = 0; i < right_schema.cell_num(); i++) {
        schema.append_cell(right_schema.cell_at(i));
    }
    return RC::SUCCESS;
}