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

#include <stdint.h>
#include <string>
#include <unordered_map>

#include "common/sys/rc.h"

class Table;
class Trx;

/**
 * Table-level statistics for optimizer.
 *
 * - row_count: number of visible rows (under the given trx/mode)
 * - used_pages: number of distinct data pages referenced by scanned rows
 * - ndv: number of distinct values per column (normal/user columns only)
 */
class TableStatistics
{
public:
    TableStatistics() = default;

    RC analyze(Table *table, Trx *trx);

    int64_t row_count() const { return row_count_; }
    int64_t used_pages() const { return used_pages_; }

    /// Returns RC::SUCCESS and sets ndv_out if column exists, otherwise RC::NOTFOUND
    RC get_ndv(const std::string &column_name, int64_t &ndv_out) const;

    const std::unordered_map<std::string, int64_t> &ndv_by_column() const { return ndv_by_column_; }

private:
    void reset();

private:
    int64_t row_count_  = 0;
    int64_t used_pages_ = 0;
    std::unordered_map<std::string, int64_t> ndv_by_column_;
};