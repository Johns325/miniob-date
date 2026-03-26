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
// Created by WangYunlai on 2021/6/9.
//

#include "sql/operator/physical/table_scan_physical_operator.h"
#include "event/sql_debug.h"
#include "catalog/catalog.h"
#include "storage/buffer/page.h"
#include "storage/table/table.h"
#include "sql/optimizer/optimizer_utils.h"

using namespace std;

size_t TableScanPhysicalOperator::estimate_output_size() const
{
  if (table_ == nullptr) {
    return 0;
  }
  const int rows = Catalog::get_instance().get_table_stats(table_->table_id()).row_nums;
  return rows < 0 ? 0 : static_cast<size_t>(rows);
}

double TableScanPhysicalOperator::calculate_cost(
    LogicalProperty *prop, const vector<LogicalProperty *> &child_log_props, CostModel *cm)
{
  (void)child_log_props;
  if (cm == nullptr || table_ == nullptr) {
    return 0.0;
  }

  int64_t rows = static_cast<int64_t>(Catalog::get_instance().get_table_stats(table_->table_id()).row_nums);
  if (rows <= 0 && prop != nullptr) {
    rows = prop->get_card();
  }
  if (rows < 0) {
    rows = 0;
  }

  const int record_size = table_->table_meta().record_size();
  int64_t pages = 0;
  if (rows > 0 && record_size > 0) {
    const int64_t bytes = rows * static_cast<int64_t>(record_size);
    pages               = (bytes + BP_PAGE_DATA_SIZE - 1) / BP_PAGE_DATA_SIZE;
  }

  return cm->seq_page_cost() * static_cast<double>(pages) + cm->cpu_tuple_cost() * static_cast<double>(rows);
}

RC TableScanPhysicalOperator::open(Trx *trx)
{
  RC rc = table_->get_record_scanner(record_scanner_, trx, mode_);
  if (rc == RC::SUCCESS) {
    tuple_.set_schema(table_, table_->table_meta().field_metas());
  }
  trx_ = trx;
  return rc;
}

RC TableScanPhysicalOperator::next()
{
  RC rc = RC::SUCCESS;

  bool filter_result = false;
  while (OB_SUCC(rc = record_scanner_->next(current_record_))) {
    LOG_TRACE("got a record. rid=%s", current_record_.rid().to_string().c_str());
    
    tuple_.set_record(&current_record_);
    rc = filter(tuple_, filter_result);
    if (rc != RC::SUCCESS) {
      LOG_TRACE("record filtered failed=%s", strrc(rc));
      return rc;
    }

    if (filter_result) {
      sql_debug("get a tuple: %s", tuple_.to_string().c_str());
      break;
    } else {
      sql_debug("a tuple is filtered: %s", tuple_.to_string().c_str());
    }
  }
  return rc;
}

RC TableScanPhysicalOperator::close() {
  RC rc = RC::SUCCESS;
  if (record_scanner_ != nullptr) {
    rc = record_scanner_->close_scan();
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to close record scanner");
    }
    delete record_scanner_;
    record_scanner_ = nullptr;
  }
  return rc;

}

Tuple *TableScanPhysicalOperator::current_tuple()
{
  tuple_.set_record(&current_record_);
  return &tuple_;
}

string TableScanPhysicalOperator::param() const
{
  string result = table_->name();
  if (!predicates_.empty()) {
    result += ", filter=";
    for (size_t i = 0; i < predicates_.size(); i++) {
      if (i > 0) {
        result += " AND ";
      }
      result += OptimizerUtils::expression_to_string(predicates_[i].get());
    }
  }
  return result;
}

void TableScanPhysicalOperator::set_predicates(vector<unique_ptr<Expression>> &&exprs)
{
  predicates_ = std::move(exprs);
}

RC TableScanPhysicalOperator::filter(RowTuple &tuple, bool &result)
{
  RC    rc = RC::SUCCESS;
  Value value;
  for (unique_ptr<Expression> &expr : predicates_) {
    rc = expr->get_value(tuple, value);
    if (rc != RC::SUCCESS) {
      return rc;
    }

    bool tmp_result = value.get_boolean();
    if (!tmp_result) {
      result = false;
      return rc;
    }
  }

  result = true;
  return rc;
}
