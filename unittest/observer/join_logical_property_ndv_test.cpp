#include <gtest/gtest.h>

#include "sql/operator/logical/join_logical_operator.h"
#include "sql/optimizer/cascade/property.h"
#include "sql/parser/parse_defs.h"

TEST(join_logical_property_ndv_test, equi_join_uses_ndv_for_cardinality)
{
  LogicalProperty left_prop(1000);
  left_prop.set_ndv("t1.a", 100);
  left_prop.set_ndv("t1.x", 500);

  LogicalProperty right_prop(5000);
  right_prop.set_ndv("t2.b", 200);
  right_prop.set_ndv("t2.y", 3000);

  JoinLogicalOperator join;
  join.add_join_predicate(std::make_unique<ComparisonExpr>(
      CompOp::EQUAL_TO,
      std::make_unique<UnboundFieldExpr>("t1", "a"),
      std::make_unique<UnboundFieldExpr>("t2", "b")));

  std::vector<LogicalProperty *> inputs{&left_prop, &right_prop};
  std::unique_ptr<LogicalProperty> out = join.find_log_prop(inputs);
  ASSERT_NE(out, nullptr);

  // |R⋈S| ≈ |R|*|S|/max(NDV(a), NDV(b)) = 1000*5000/max(100,200)=25000
  EXPECT_EQ(out->get_card(), 25000);

  int64_t ndv_a = 0;
  int64_t ndv_b = 0;
  EXPECT_TRUE(out->get_ndv("t1.a", ndv_a));
  EXPECT_TRUE(out->get_ndv("t2.b", ndv_b));
  EXPECT_EQ(ndv_a, 100);
  EXPECT_EQ(ndv_b, 100);

  // Non-join columns should be propagated and capped by output cardinality.
  int64_t ndv_x = 0;
  int64_t ndv_y = 0;
  EXPECT_TRUE(out->get_ndv("t1.x", ndv_x));
  EXPECT_TRUE(out->get_ndv("t2.y", ndv_y));
  EXPECT_LE(ndv_x, out->get_card());
  EXPECT_LE(ndv_y, out->get_card());
}
