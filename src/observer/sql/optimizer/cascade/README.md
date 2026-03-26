# Cascade Optimizer
设计文档请参考 `docs/docs/design/miniob-cascade.md`。

主设计文档：`docs/docs/design/miniob-cascade.md`。

代码入口与关键文件：
- SQL → 优化：`src/observer/sql/optimizer/optimize_stage.cpp`
- Stmt → 逻辑计划(GroupExpr)：`src/observer/sql/optimizer/logical_plan_generator.cpp`
- 优化器入口(CBO/RBO)：`src/observer/sql/optimizer/cascade/optimizer.cpp`
- Memo/Group/GroupExpr：`src/observer/sql/optimizer/cascade/{memo,group,group_expr}.h`
- 规则注册：`src/observer/sql/optimizer/cascade/rules.cpp`
- Tasks：`src/observer/sql/optimizer/cascade/tasks/`
