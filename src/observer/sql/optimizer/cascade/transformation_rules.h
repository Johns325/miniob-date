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

#include "sql/optimizer/cascade/rules.h"

class Expression;

/**
 * Rule transforms Filter(TableGet) -> TableGet with predicates
 * Pushes predicates from Filter down to TableGet operator
 */
class PredicatePushdownRule : public Rule
{
public:
  PredicatePushdownRule();

  void transform(
      GroupExpr *input, std::vector<CandidateExpression> *transformed, OptimizerContext *context) const override;
};

/**
 * Rule transforms Filter(Filter(...)) -> Filter(...) or removes Filter
 * Removes constant true/false predicates
 */
class PredicateRewriteRule : public Rule
{
public:
  PredicateRewriteRule();

  void transform(
      GroupExpr *input, std::vector<CandidateExpression> *transformed, OptimizerContext *context) const override;
};

/**
 * Rule simplifies expressions in logical operators
 * Simplifies comparison and conjunction expressions
 */
class ExpressionSimplifyRule : public Rule
{
public:
  ExpressionSimplifyRule();

  void transform(
      GroupExpr *input, std::vector<CandidateExpression> *transformed, OptimizerContext *context) const override;

private:
  // Simplify expression, returns true if changed
  bool simplify_expression(unique_ptr<Expression> &expr) const;
};

class JoinCommutativityRule : public Rule
{
public:
  JoinCommutativityRule();
  void transform(
      GroupExpr *input, std::vector<CandidateExpression> *transformed, OptimizerContext *context) const override;
};

class JoinAssociativityRule : public Rule
{
public:
  JoinAssociativityRule();
  void transform(
      GroupExpr *input, std::vector<CandidateExpression> *transformed, OptimizerContext *context) const override;
};

/**
 * Rule transforms Limit(OrderBy(X)) -> TopN(X)
 */
class TopNRule : public Rule
{
public:
  TopNRule();

  void transform(
      GroupExpr *input, std::vector<CandidateExpression> *transformed, OptimizerContext *context) const override;
};

/**
 * Rule transforms ScalarGroupBy(Join(L, R)) -> ScalarGroupBy(Join(L, HashGroupBy(R))) (or symmetric)
 *
 * Current simplified version:
 * - Only scalar aggregation (no GROUP BY at the top)
 * - Only INNER equi-join predicates (possibly conjunctions)
 * - Only pushes aggregation to one side (chosen based on aggregate column references)
 * - Supports COUNT(*) and SUM/MIN/MAX on one side; AVG/COUNT(col) are not rewritten.
 */
class AggregateJoinPushdownRule : public Rule
{
public:
  AggregateJoinPushdownRule();

  void transform(
      GroupExpr *input, std::vector<CandidateExpression> *transformed, OptimizerContext *context) const override;
};