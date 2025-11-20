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
    // LAB3 TODO
    /*
      遍历 oper 的子树，收集所有 TableGetLogicalOperator 实例到 table_get_ops 向量中
    */
}

RC PredicateToJoinRewriter::rewrite(unique_ptr<LogicalOperator> &oper, bool &change_made)
{
    // printf("PredicateToJoinRewriter::rewrite called\n");
    if (oper == nullptr || oper->type() != LogicalOperatorType::PREDICATE) {
        return RC::SUCCESS;
    }

    // LAB3 TODO
    /*
      实现谓词下推到连接的重写逻辑
      包括下推到 TableGetLogicalOperator 和 JoinLogicalOperator 两种情况
      如果进行了任何重写，设置 change_made 为 true
    */

    return RC::SUCCESS;
}

