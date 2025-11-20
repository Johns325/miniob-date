/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/optimizer/predicate_to_join_rule.h"
#include "sql/operator/logical_operator.h"
#include "sql/operator/predicate_logical_operator.h"
#include "sql/operator/table_get_logical_operator.h"
#include "sql/expr/expression.h"
#include "sql/operator/join_logical_operator.h"

#include <string>
#include <unordered_map>

using namespace std;


void PredicateToJoinRewriter::visitor(LogicalOperator *oper, std::vector<TableGetLogicalOperator *> &table_get_ops)
{
    if (oper == nullptr) return;
    printf("Visiting operator type=%d\n", static_cast<int>(oper->type()));
    if (oper->type() == LogicalOperatorType::TABLE_GET) {
        table_get_ops.emplace_back(dynamic_cast<TableGetLogicalOperator *>(oper));
        return;
    }
    for (auto &child : oper->children()) {
        visitor(child.get(), table_get_ops);
    }
}

RC PredicateToJoinRewriter::rewrite(unique_ptr<LogicalOperator> &oper, bool &change_made)
{
    // printf("PredicateToJoinRewriter::rewrite called\n");
    if (oper == nullptr || oper->type() != LogicalOperatorType::PREDICATE) {
        return RC::SUCCESS;
    }

    auto pred_oper = dynamic_cast<PredicateLogicalOperator *>(oper.get());
    if (pred_oper == nullptr) return RC::SUCCESS;

    // collect all TableGet under this predicate
    std::vector<TableGetLogicalOperator *> table_get_ops;
    visitor(oper.get(), table_get_ops);

    // classify predicates: pushdown per table, join predicates, remaining
    unordered_map<string, vector<unique_ptr<Expression>>> pushdown_map;
    vector<unique_ptr<Expression>> join_preds;
    vector<unique_ptr<Expression>> remain_preds;

    // iterate over a copy of expressions (we will rebuild target list)
    for (auto &expr_ptr : pred_oper->expressions()) {
        if (expr_ptr == nullptr) continue;

        // If this is a conjunction, iterate its children
        if (expr_ptr->type() == ExprType::CONJUNCTION) {
            auto conj = dynamic_cast<ConjunctionExpr *>(expr_ptr.get());
            if (conj) {
                for (auto &child : conj->children()) {
                    if (child == nullptr) continue;
                    if (child->type() != ExprType::COMPARISON) {
                        remain_preds.emplace_back(child->copy());
                        continue;
                    }
                    auto cmp = dynamic_cast<ComparisonExpr *>(child.get());
                    if (cmp == nullptr) {
                        remain_preds.emplace_back(child->copy());
                        continue;
                    }

                    if (cmp->field_value_comparison()) {
                        FieldExpr *field_expr = nullptr;
                        if (cmp->left()->type() == ExprType::FIELD) field_expr = dynamic_cast<FieldExpr *>(cmp->left().get());
                        else if (cmp->right()->type() == ExprType::FIELD) field_expr = dynamic_cast<FieldExpr *>(cmp->right().get());

                        if (field_expr != nullptr) {
                            string tname(field_expr->table_name());
                            bool pushed = false;
                            for (auto tget : table_get_ops) {
                                if (tget != nullptr && tname == string(tget->table()->name())) {
                                    pushdown_map[tname].emplace_back(child->copy());
                                    pushed = true;
                                    break;
                                }
                            }
                            if (!pushed) remain_preds.emplace_back(child->copy());
                        } else {
                            remain_preds.emplace_back(child->copy());
                        }
                    } else if (cmp->field_field_comparison()) {
                        FieldExpr *l = dynamic_cast<FieldExpr *>(cmp->left().get());
                        FieldExpr *r = dynamic_cast<FieldExpr *>(cmp->right().get());
                        if (l && r) {
                            string lt(l->table_name()), rt(r->table_name());
                            if (lt != rt) {
                                join_preds.emplace_back(child->copy());
                            } else {
                                pushdown_map[lt].emplace_back(child->copy());
                            }
                        } else {
                            remain_preds.emplace_back(child->copy());
                        }
                    } else {
                        remain_preds.emplace_back(child->copy());
                    }
                }
                continue;
            }
        }

        // non-conjunction (single expression)
        if (expr_ptr->type() != ExprType::COMPARISON) {
            remain_preds.emplace_back(expr_ptr->copy());
            continue;
        }

        auto cmp = dynamic_cast<ComparisonExpr *>(expr_ptr.get());
        if (cmp == nullptr) {
            remain_preds.emplace_back(expr_ptr->copy());
            continue;
        }

        if (cmp->field_value_comparison()) {
            FieldExpr *field_expr = nullptr;
            if (cmp->left()->type() == ExprType::FIELD) field_expr = dynamic_cast<FieldExpr *>(cmp->left().get());
            else if (cmp->right()->type() == ExprType::FIELD) field_expr = dynamic_cast<FieldExpr *>(cmp->right().get());

            if (field_expr != nullptr) {
                string tname(field_expr->table_name());
                bool pushed = false;
                for (auto tget : table_get_ops) {
                    if (tget != nullptr && tname == string(tget->table()->name())) {
                        pushdown_map[tname].emplace_back(expr_ptr->copy());
                        pushed = true;
                        break;
                    }
                }
                if (!pushed) remain_preds.emplace_back(expr_ptr->copy());
            } else {
                remain_preds.emplace_back(expr_ptr->copy());
            }
        } else if (cmp->field_field_comparison()) {
            FieldExpr *l = dynamic_cast<FieldExpr *>(cmp->left().get());
            FieldExpr *r = dynamic_cast<FieldExpr *>(cmp->right().get());
            if (l && r) {
                string lt(l->table_name()), rt(r->table_name());
                if (lt != rt) {
                    join_preds.emplace_back(expr_ptr->copy());
                } else {
                    pushdown_map[lt].emplace_back(expr_ptr->copy());
                }
            } else {
                remain_preds.emplace_back(expr_ptr->copy());
            }
        } else {
            remain_preds.emplace_back(expr_ptr->copy());
        }
    }

    // apply pushdowns
    for (auto tget : table_get_ops) {
        if (tget == nullptr) continue;
        string name(tget->table()->name());
        auto it = pushdown_map.find(name);
        if (it != pushdown_map.end()) {
            for (auto &e : it->second) {
                if (!e) continue;
                bool already = false;
                for (auto &exist : tget->predicates()) {
                    if (exist && exist->equal(*e)) {
                        already = true;
                        break;
                    }
                }
                if (already) continue;
                printf("predicate pushdown\n");
                // push down into TableGetLogicalOperator's predicates so the
                // PhysicalPlanGenerator can see them (TableGet has its own
                // predicates_ member separate from LogicalOperator::expressions_)
                tget->predicates().emplace_back(std::move(e));
                change_made = true;
            }
        }
    }

    // If we've pushed down everything (no remaining preds and no join preds),
    // and predicate has a single child, remove this predicate node by
    // replacing it with its child so EXPLAIN/physical generator won't show
    // a no-op PREDICATE node.
    if (join_preds.empty()) {
        bool has_nonempty_remain = false;
        for (auto &rp : remain_preds) {
            if (rp) { has_nonempty_remain = true; break; }
        }
        if (!has_nonempty_remain) {
            if (oper->children().size() == 1) {
                printf("removing empty predicate node and replacing with its child\n");
                oper = std::move(oper->children()[0]);
                change_made = true;
                return RC::SUCCESS;
            }
        }
    }

    // try to convert to join if there are join predicates
    if (!join_preds.empty()) {
        // case A: predicate directly above an existing Join operator -> attach predicates to it
        if (oper->children().size() == 1 && oper->children()[0]->type() == LogicalOperatorType::JOIN) {
            auto join_child = dynamic_cast<JoinLogicalOperator *>(oper->children()[0].get());
            if (join_child) {
                // wrap join_preds into a conjunction and add to existing join
                std::vector<std::unique_ptr<Expression>> jp_children;
                for (auto &jp : join_preds) {
                    if (jp) {
                        printf("join predicate pushdown\n");
                        jp_children.emplace_back(std::move(jp));
                    }
                }
                if (!jp_children.empty()) {
                    unique_ptr<ConjunctionExpr> conj(new ConjunctionExpr(ConjunctionExpr::Type::AND, jp_children));
                    // avoid duplicate attach
                    bool exists = false;
                    for (auto &exist_pred : join_child->get_join_predicates()) {
                        if (exist_pred && exist_pred->equal(*conj)) {
                            exists = true;
                            break;
                        }
                    }
                    if (!exists) {
                        join_child->add_join_predicate(std::move(conj));
                        change_made = true;
                    }
                }
                // remove join_preds from predicate (they were consumed)
                join_preds.clear();
                // if predicate has no remaining predicates and only one child, remove it now
                bool has_nonempty_remain = false;
                for (auto &rp : remain_preds) {
                    if (rp) { has_nonempty_remain = true; break; }
                }
                if (!has_nonempty_remain && oper->children().size() == 1) {
                    oper = std::move(oper->children()[0]);
                    change_made = true;
                    return RC::SUCCESS;
                }
            }
        }

        // try to convert to join if there are exactly two direct table children and we have join predicates
        if (!join_preds.empty()) {
            // gather direct table children (immediate children of this predicate)
            vector<int> table_child_indices;
            for (size_t i = 0; i < oper->children().size(); i++) {
                if (oper->children()[i]->type() == LogicalOperatorType::TABLE_GET) {
                    table_child_indices.push_back(static_cast<int>(i));
                }
            }
        
            if (table_child_indices.size() == 2) {
                // create join op and move the two table children into it
                auto join_op = make_unique<JoinLogicalOperator>();
                // move higher index first to keep vector indices valid
                int idx1 = table_child_indices[0];
                int idx2 = table_child_indices[1];
                if (idx1 > idx2) std::swap(idx1, idx2);
                // move child at idx2 then idx1
                printf("converting predicate+two-table children into Join: idx1=%d idx2=%d\n", idx1, idx2);
                // defensive checks
                if (idx1 < 0 || idx2 < 0 || idx1 >= (int)oper->children().size() || idx2 >= (int)oper->children().size()) {
                    printf("invalid child indices for join conversion\n");
                } else {
                    join_op->add_child(std::move(oper->children()[idx2]));
                    join_op->add_child(std::move(oper->children()[idx1]));
                }

                // attach join predicates: wrap them into a single ConjunctionExpr so
                // downstream expects one conjunction expression for join predicates
                {
                    std::vector<std::unique_ptr<Expression>> jp_children;
                    for (auto &jp : join_preds) {
                        if (jp) {
                            printf("join predicate pushdown\n");
                            jp_children.emplace_back(std::move(jp));
                        }
                    }
                    if (!jp_children.empty()) {
                        unique_ptr<ConjunctionExpr> conj(new ConjunctionExpr(ConjunctionExpr::Type::AND, jp_children));
                        join_op->add_join_predicate(std::move(conj));
                        change_made = true;
                    }
                }

                // replace current predicate node with join_op
                oper = std::unique_ptr<LogicalOperator>(std::move(join_op));
                return RC::SUCCESS;
            }
            else {
                // cannot convert to join here, keep join preds on predicate
                for (auto &jp : join_preds) {
                    if (jp) {
                        remain_preds.emplace_back(std::move(jp));
                    }
                }
            }
        }
    
        // rebuild predicate expressions: wrap remaining predicates into one ConjunctionExpr
        std::vector<std::unique_ptr<Expression>> tmp_children;
        for (auto &rp : remain_preds) {
            if (rp) tmp_children.emplace_back(std::move(rp));
        }
        // create conjunction (AND) containing remaining predicates (may be empty)
        unique_ptr<ConjunctionExpr> conjunction_expr(new ConjunctionExpr(ConjunctionExpr::Type::AND, tmp_children));
        pred_oper->expressions().clear();
        pred_oper->expressions().emplace_back(std::move(conjunction_expr));
    }

    return RC::SUCCESS;
}

