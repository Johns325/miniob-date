/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/optimizer/statistics/table_statistics.h"

#include <unordered_set>

#include "common/log/log.h"
#include "storage/record/record.h"
#include "storage/record/record_scanner.h"
#include "storage/table/table.h"
#include "storage/table/table_meta.h"
#include "storage/trx/trx.h"

void TableStatistics::reset()
{
  row_count_  = 0;
  used_pages_ = 0;
  ndv_by_column_.clear();
}

RC TableStatistics::get_ndv(const std::string &column_name, int64_t &ndv_out) const
{
  auto it = ndv_by_column_.find(column_name);
  if (it == ndv_by_column_.end()) {
    return RC::NOTFOUND;
  }
  ndv_out = it->second;
  return RC::SUCCESS;
}

RC TableStatistics::analyze(Table *table, Trx *trx)
{
  reset();

  if (table == nullptr) {
    return RC::INVALID_ARGUMENT;
  }

  const TableMeta &meta          = table->table_meta();
  const int        sys_field_num = meta.sys_field_num();
  const int        field_num     = meta.field_num();

  if (field_num < sys_field_num) {
    return RC::INTERNAL;
  }

  const int normal_field_num = field_num - sys_field_num;

  // Distinct sets per column (normal columns only)
  std::vector<std::unordered_set<std::string>> distinct_sets;
  distinct_sets.resize(normal_field_num);

  std::unordered_set<PageNum> distinct_pages;

  RecordScanner *scanner = nullptr;
  RC            rc       = table->get_record_scanner(scanner, trx, ReadWriteMode::READ_ONLY);
  if (OB_FAIL(rc)) {
    return rc;
  }
  if (scanner == nullptr) {
    return RC::INTERNAL;
  }

  Record record;
  while (OB_SUCC(rc = scanner->next(record))) {
    row_count_++;

    // Page counting is only meaningful for HEAP storage.
    if (meta.storage_engine() == StorageEngine::HEAP) {
      const PageNum page_num = record.rid().page_num;
      if (page_num > 0) {
        distinct_pages.insert(page_num);
      }
    }

    const char *data = record.data();
    if (data == nullptr) {
      // should not happen for heap, but keep robust
      continue;
    }

    for (int i = 0; i < normal_field_num; i++) {
      const FieldMeta *field = meta.field(i + sys_field_num);
      if (field == nullptr) {
        continue;
      }

      const char *ptr = data + field->offset();
      int         len = field->len();

      // Use raw bytes as distinct key. For CHARS, trim trailing zeros by bounded strnlen.
      std::string key;
      if (field->type() == AttrType::CHARS) {
        const int actual = static_cast<int>(strnlen(ptr, len));
        key.assign(ptr, actual);
      } else {
        key.assign(ptr, len);
      }

      distinct_sets[i].insert(std::move(key));
    }
  }

  if (rc != RC::RECORD_EOF) {
    scanner->close_scan();
    delete scanner;
    return rc;
  }

  rc = scanner->close_scan();
  delete scanner;
  if (OB_FAIL(rc)) {
    return rc;
  }

  used_pages_ = static_cast<int64_t>(distinct_pages.size());

  // Materialize NDV results
  for (int i = 0; i < normal_field_num; i++) {
    const FieldMeta *field = meta.field(i + sys_field_num);
    if (field == nullptr) {
      continue;
    }
    ndv_by_column_[field->name()] = static_cast<int64_t>(distinct_sets[i].size());
  }

  return RC::SUCCESS;
}
