/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once

#include "sql/operator/physical_operator.h"

#include <unordered_map>

#include "common/log/log.h"
#include "common/value.h"
#include "sql/expr/expression.h"
#include "sql/expr/tuple.h"
#include "sql/parser/parse.h"

/**
 * @brief Hash Join 算子
 * @ingroup PhysicalOperator
 */
class HashJoinPhysicalOperator : public PhysicalOperator
{
public:
    HashJoinPhysicalOperator() = default;
    explicit HashJoinPhysicalOperator(vector<unique_ptr<Expression>> &&join_predicates)
        : join_predicates_(std::move(join_predicates))
    {}
    virtual ~HashJoinPhysicalOperator() = default;

    OpType get_op_type() const override { return OpType::INNERHASHJOIN; }

    size_t estimate_output_size() const override;

    double calculate_cost(LogicalProperty *prop, const vector<LogicalProperty *> &child_log_props, CostModel *cm) override;

    RC     open(Trx *trx) override;
    RC     next() override;
    RC     close() override;
    Tuple *current_tuple() override;
    RC     tuple_schema(TupleSchema &schema) const override;

private:
    struct JoinKey
    {
        vector<Value> values;
    };

    struct JoinKeyHasher
    {
        size_t operator()(const JoinKey &key) const noexcept;
    };

    struct JoinKeyEqual
    {
        bool operator()(const JoinKey &a, const JoinKey &b) const noexcept;
    };

    struct KeyExprPair
    {
        const Expression *probe_expr = nullptr;  // evaluated on left child
        const Expression *build_expr = nullptr;  // evaluated on right child
    };

private:
    RC init_key_exprs_with_right_tuple(const Tuple &right_tuple);
    RC make_key(const Tuple &tuple, const vector<const Expression *> &exprs, JoinKey &key) const;
    RC make_probe_key(const Tuple &left_tuple, JoinKey &key) const;
    RC make_build_key(const Tuple &right_tuple, JoinKey &key) const;
    bool eval_residual_predicates(const Tuple &left_tuple, const Tuple &right_tuple) const;

private:
    Trx *trx_ = nullptr;

    PhysicalOperator *left_  = nullptr;
    PhysicalOperator *right_ = nullptr;

    Tuple *left_tuple_ = nullptr;
    JoinedTuple joined_tuple_;

    vector<unique_ptr<Expression>> join_predicates_;
    vector<KeyExprPair>            key_expr_pairs_;
    vector<const Expression *>     probe_key_exprs_;
    vector<const Expression *>     build_key_exprs_;
    bool                           key_exprs_ready_ = false;

    vector<ValueListTuple> right_rows_;
    std::unordered_multimap<JoinKey, size_t, JoinKeyHasher, JoinKeyEqual> hash_table_;

    // iteration state for current probe key
    std::pair<decltype(hash_table_)::const_iterator, decltype(hash_table_)::const_iterator> match_range_;
    decltype(hash_table_)::const_iterator                                                   match_iter_;
    bool                                                                                     have_active_matches_ = false;
};