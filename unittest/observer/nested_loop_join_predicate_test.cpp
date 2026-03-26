/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

#include "common/value.h"
#include "sql/expr/expression.h"
#include "sql/expr/tuple.h"
#include "sql/operator/physical/nested_loop_join_physical_operator.h"
#include "sql/operator/physical_operator.h"
#include "sql/parser/parse_defs.h"

using namespace std;

namespace {

class VectorTuplePhysicalOperator : public PhysicalOperator
{
public:
  explicit VectorTuplePhysicalOperator(vector<ValueListTuple> rows) : rows_(std::move(rows)) {}

  OpType get_op_type() const override { return OpType::SEQSCAN; }

  RC open(Trx * /*trx*/) override
  {
    opened_ = true;
    idx_    = 0;
    cur_    = nullptr;
    return RC::SUCCESS;
  }

  RC next() override
  {
    if (!opened_) {
      return RC::INTERNAL;
    }
    if (idx_ >= rows_.size()) {
      cur_ = nullptr;
      return RC::RECORD_EOF;
    }
    cur_ = &rows_[idx_++];
    return RC::SUCCESS;
  }

  RC close() override
  {
    opened_ = false;
    cur_    = nullptr;
    return RC::SUCCESS;
  }

  Tuple *current_tuple() override { return cur_; }

  size_t estimate_output_size() const override { return rows_.size(); }

private:
  vector<ValueListTuple> rows_;
  size_t                idx_    = 0;
  bool                  opened_ = false;
  ValueListTuple        *cur_   = nullptr;
};

class TestCellExpr : public Expression
{
public:
  TestCellExpr(const char *table_name, const char *field_name, AttrType type)
      : spec_(table_name, field_name), type_(type)
  {}

  unique_ptr<Expression> copy() const override
  {
    return make_unique<TestCellExpr>(spec_.table_name(), spec_.field_name(), type_);
  }

  RC get_value(const Tuple &tuple, Value &value) const override { return tuple.find_cell(spec_, value); }

  ExprType type() const override { return ExprType::FIELD; }

  AttrType value_type() const override { return type_; }

private:
  TupleCellSpec spec_;
  AttrType      type_;
};

static ValueListTuple make_row(const char *table_name, const vector<pair<const char *, Value>> &cols)
{
  vector<TupleCellSpec> specs;
  vector<Value>         values;
  specs.reserve(cols.size());
  values.reserve(cols.size());

  for (const auto &kv : cols) {
    specs.emplace_back(table_name, kv.first);
    values.push_back(kv.second);
  }

  ValueListTuple t;
  t.set_names(specs);
  t.set_cells(values);
  return t;
}

static Value int_v(int v)
{
  Value x;
  x.set_int(v);
  return x;
}

static size_t run_and_count(NestedLoopJoinPhysicalOperator &nlj)
{
  EXPECT_EQ(RC::SUCCESS, nlj.open(nullptr));

  size_t cnt = 0;
  while (true) {
    RC rc = nlj.next();
    if (rc == RC::RECORD_EOF) {
      break;
    }
    EXPECT_EQ(RC::SUCCESS, rc);
    Tuple *t = nlj.current_tuple();
    EXPECT_NE(nullptr, t);
    cnt++;
  }

  EXPECT_EQ(RC::SUCCESS, nlj.close());
  return cnt;
}

}  // namespace

TEST(NestedLoopJoinPredicate, no_predicates_outputs_cartesian)
{
  vector<ValueListTuple> left_rows;
  left_rows.emplace_back(make_row("t1", {{"a", int_v(1)}}));
  left_rows.emplace_back(make_row("t1", {{"a", int_v(2)}}));

  vector<ValueListTuple> right_rows;
  right_rows.emplace_back(make_row("t2", {{"b", int_v(10)}}));
  right_rows.emplace_back(make_row("t2", {{"b", int_v(20)}}));
  right_rows.emplace_back(make_row("t2", {{"b", int_v(30)}}));

  NestedLoopJoinPhysicalOperator nlj;
  nlj.add_child(make_unique<VectorTuplePhysicalOperator>(left_rows));
  nlj.add_child(make_unique<VectorTuplePhysicalOperator>(right_rows));

  const size_t out = run_and_count(nlj);
  ASSERT_EQ(2U * 3U, out);
}

TEST(NestedLoopJoinPredicate, equi_predicate_filters_output)
{
  vector<ValueListTuple> left_rows;
  left_rows.emplace_back(make_row("t1", {{"a", int_v(1)}}));
  left_rows.emplace_back(make_row("t1", {{"a", int_v(2)}}));

  vector<ValueListTuple> right_rows;
  right_rows.emplace_back(make_row("t2", {{"b", int_v(1)}}));
  right_rows.emplace_back(make_row("t2", {{"b", int_v(1)}}));
  right_rows.emplace_back(make_row("t2", {{"b", int_v(3)}}));

  vector<unique_ptr<Expression>> preds;
  preds.emplace_back(make_unique<ComparisonExpr>(
      CompOp::EQUAL_TO,
      make_unique<TestCellExpr>("t1", "a", AttrType::INTS),
      make_unique<TestCellExpr>("t2", "b", AttrType::INTS)));

  NestedLoopJoinPhysicalOperator nlj(std::move(preds));
  nlj.add_child(make_unique<VectorTuplePhysicalOperator>(left_rows));
  nlj.add_child(make_unique<VectorTuplePhysicalOperator>(right_rows));

  ASSERT_EQ(2U, run_and_count(nlj));
}

TEST(NestedLoopJoinPredicate, multi_predicates_and_semantics)
{
  // Predicates: t1.a = t2.b AND t1.c < t2.d
  vector<ValueListTuple> left_rows;
  left_rows.emplace_back(make_row("t1", {{"a", int_v(1)}, {"c", int_v(5)}}));
  left_rows.emplace_back(make_row("t1", {{"a", int_v(1)}, {"c", int_v(9)}}));
  left_rows.emplace_back(make_row("t1", {{"a", int_v(2)}, {"c", int_v(1)}}));

  vector<ValueListTuple> right_rows;
  right_rows.emplace_back(make_row("t2", {{"b", int_v(1)}, {"d", int_v(6)}}));   // matches left (1,5)
  right_rows.emplace_back(make_row("t2", {{"b", int_v(1)}, {"d", int_v(8)}}));   // matches left (1,5) only
  right_rows.emplace_back(make_row("t2", {{"b", int_v(1)}, {"d", int_v(10)}}));  // matches left (1,5) and (1,9)
  right_rows.emplace_back(make_row("t2", {{"b", int_v(2)}, {"d", int_v(1)}}));   // no match (strict <)

  vector<unique_ptr<Expression>> preds;
  preds.emplace_back(make_unique<ComparisonExpr>(
      CompOp::EQUAL_TO,
      make_unique<TestCellExpr>("t1", "a", AttrType::INTS),
      make_unique<TestCellExpr>("t2", "b", AttrType::INTS)));
  preds.emplace_back(make_unique<ComparisonExpr>(
      CompOp::LESS_THAN,
      make_unique<TestCellExpr>("t1", "c", AttrType::INTS),
      make_unique<TestCellExpr>("t2", "d", AttrType::INTS)));

  NestedLoopJoinPhysicalOperator nlj(std::move(preds));
  nlj.add_child(make_unique<VectorTuplePhysicalOperator>(left_rows));
  nlj.add_child(make_unique<VectorTuplePhysicalOperator>(right_rows));

  // Expected matches:
  // left(1,5): right d=6,8,10 -> 3
  // left(1,9): right d=10 -> 1
  // left(2,1): right(b=2,d=1) but 1<1 false -> 0
  ASSERT_EQ(4U, run_and_count(nlj));
}

TEST(NestedLoopJoinPredicate, empty_inputs_produce_no_rows)
{
  {
    vector<ValueListTuple> left_rows;
    vector<ValueListTuple> right_rows;
    right_rows.emplace_back(make_row("t2", {{"b", int_v(1)}}));

    NestedLoopJoinPhysicalOperator nlj;
    nlj.add_child(make_unique<VectorTuplePhysicalOperator>(left_rows));
    nlj.add_child(make_unique<VectorTuplePhysicalOperator>(right_rows));
    ASSERT_EQ(0U, run_and_count(nlj));
  }

  {
    vector<ValueListTuple> left_rows;
    left_rows.emplace_back(make_row("t1", {{"a", int_v(1)}}));
    vector<ValueListTuple> right_rows;

    NestedLoopJoinPhysicalOperator nlj;
    nlj.add_child(make_unique<VectorTuplePhysicalOperator>(left_rows));
    nlj.add_child(make_unique<VectorTuplePhysicalOperator>(right_rows));
    ASSERT_EQ(0U, run_and_count(nlj));
  }
}
