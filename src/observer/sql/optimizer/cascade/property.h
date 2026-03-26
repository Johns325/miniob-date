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

class Property
{};

/**
 * @brief Logical Property, such as the cardinality of logical operator
 */
class LogicalProperty
{
public:
  // Default cardinality when property is unknown
  static constexpr int DEFAULT_CARDINALITY = 1000;

  explicit LogicalProperty(int card) : card_(card) {}
  LogicalProperty()  = default;
  ~LogicalProperty() = default;

  int get_card() const { return card_; }

  void set_card(int card) { card_ = card; }

  /// Set NDV (number of distinct values) for a column key (e.g. "table.col")
  void set_ndv(const std::string &column_key, int64_t ndv)
  {
    if (column_key.empty() || ndv <= 0) {
      return;
    }
    ndv_by_column_[column_key] = ndv;
  }

  /// Returns true and sets ndv_out if present
  bool get_ndv(const std::string &column_key, int64_t &ndv_out) const
  {
    auto it = ndv_by_column_.find(column_key);
    if (it == ndv_by_column_.end()) {
      return false;
    }
    ndv_out = it->second;
    return true;
  }

  const std::unordered_map<std::string, int64_t> &ndv_by_column() const { return ndv_by_column_; }

  /// Merge NDV map from another property (overwrites on key conflict)
  void merge_ndv_from(const LogicalProperty &other)
  {
    for (const auto &kv : other.ndv_by_column_) {
      ndv_by_column_[kv.first] = kv.second;
    }
  }

  /// Cap all NDVs by current cardinality (simple sanity bound)
  void cap_ndv_by_card()
  {
    const int64_t cap = card_ > 0 ? static_cast<int64_t>(card_) : 1;
    for (auto &kv : ndv_by_column_) {
      if (kv.second > cap) {
        kv.second = cap;
      }
      if (kv.second <= 0) {
        kv.second = 1;
      }
    }
  }

private:
  int card_ = 0;  /// cardinality

  /// NDV keyed by "table.column" (or other stable identifier)
  std::unordered_map<std::string, int64_t> ndv_by_column_;
};