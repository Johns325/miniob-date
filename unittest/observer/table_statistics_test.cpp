/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "gtest/gtest.h"

#include "common/value.h"
#include "sql/optimizer/statistics/table_statistics.h"
#include "storage/db/db.h"
#include "storage/record/record.h"
#include "storage/table/table.h"
#include "storage/trx/trx.h"

using namespace std;
using namespace common;

TEST(TableStatistics, basic_rowcount_pages_ndv)
{
  filesystem::path test_directory("table_statistics_test");
  filesystem::remove_all(test_directory);
  filesystem::create_directory(test_directory);

  const char *dbname            = "test_db";
  filesystem::path db_path      = test_directory / dbname;
  const char *trx_kit_name      = "mvcc";
  const char *log_handler_name  = "disk";
  filesystem::create_directories(db_path);

  auto db = make_unique<Db>();
  ASSERT_EQ(RC::SUCCESS, db->init(dbname, db_path.c_str(), trx_kit_name, log_handler_name));

  vector<AttrInfoSqlNode> attrs;
  {
    AttrInfoSqlNode a;
    a.name   = "a";
    a.type   = AttrType::INTS;
    a.length = 4;
    attrs.push_back(a);
  }
  {
    AttrInfoSqlNode b;
    b.name   = "b";
    b.type   = AttrType::CHARS;
    b.length = 8;
    attrs.push_back(b);
  }

  const char *table_name = "t";
  ASSERT_EQ(RC::SUCCESS, db->create_table(table_name, attrs, {}));
  ASSERT_EQ(RC::SUCCESS, db->sync());

  Table *table = db->find_table(table_name);
  ASSERT_NE(table, nullptr);

  TrxKit &trx_kit = db->trx_kit();
  Trx    *trx     = trx_kit.create_trx(db->log_handler());
  ASSERT_NE(trx, nullptr);
  trx->start_if_need();

  struct Row
  {
    int         a;
    const char *b;
  };
  const vector<Row> rows = {
      {1, "aa"},
      {1, "bb"},
      {2, "aa"},
      {2, "cc"},
      {3, "cc"},
  };

  for (const Row &r : rows) {
    vector<Value> values(2);
    values[0].set_int(r.a);
    values[1].set_string(r.b);

    Record record;
    ASSERT_EQ(RC::SUCCESS, table->make_record(values.size(), values.data(), record));
    ASSERT_EQ(RC::SUCCESS, trx->insert_record(table, record));
  }

  ASSERT_EQ(RC::SUCCESS, trx->commit());
  trx_kit.destroy_trx(trx);
  trx = nullptr;

  TableStatistics stats;
  ASSERT_EQ(RC::SUCCESS, stats.analyze(table, nullptr));
  ASSERT_EQ(static_cast<int64_t>(rows.size()), stats.row_count());

  ASSERT_GE(stats.used_pages(), 1);
  ASSERT_LE(stats.used_pages(), stats.row_count());

  int64_t ndv_a = 0;
  int64_t ndv_b = 0;
  ASSERT_EQ(RC::SUCCESS, stats.get_ndv("a", ndv_a));
  ASSERT_EQ(RC::SUCCESS, stats.get_ndv("b", ndv_b));
  ASSERT_EQ(3, ndv_a);
  ASSERT_EQ(3, ndv_b);
}
