# 实验手册：在 Cascade 框架中实现两个优化规则（任务 1：TopN / 任务 2：聚合下推）

> 实验目标：同学们基于给定骨架代码，在 MiniOB 的 Cascade 优化框架中实现两个规则优化。
>
> 本文先给出前置知识与任务拆分，并完整描述两个任务：**任务 1：TopN** 与 **任务 2：聚合下推**。

---

## 0. 实验提供的骨架代码（你需要做什么、不需要做什么）

我们会提前提供一份骨架代码，已经具备：

- SQL 基础执行能力：`ORDER BY`、`LIMIT`、`JOIN`、`AGGREGATION/GROUP BY` 等算子链路可用
- Cascade 优化框架：Memo + 规则（Rule）+ Pattern 匹配 + 逻辑改写（Transformation）+ 物理实现（Implementation）
- EXPLAIN：可以查看优化器选择的物理计划（physical plan）

**你需要实现的内容只有两个优化规则**：

- 任务 1：实现 `TopN` 算子与对应的规则（`Limit(OrderBy(X)) -> TopN(X)`）
- 任务 2：聚合下推（标量聚合下推到 Join 一侧）

---

## 1. 前置知识：RBO 与 Cascade 优化框架

### 1.1 RBO（Rule-Based Optimization）是什么？

RBO：基于“规则”的优化。

- 通过**等价变换**把原始逻辑计划改写成更容易高效执行的逻辑计划
- 常见形式：`A(B(x)) -> C(x)`，例如把 `Limit(OrderBy(x))` 改写成 `TopN(x)`
- 优点：实现简单、可控、对特定场景收益大
- 局限：规则触发条件需要谨慎；不同规则组合可能影响计划空间

> 本实验的 TopN 规则是典型 RBO：减少排序开销。

### 1.2 CBO 与 RBO 的关系（你在实验里会看到什么）

CBO（Cost-Based Optimization）会在多个候选计划中基于代价（cost）选最优。

在 Cascade 中通常是：

1. 先用 RBO 规则做逻辑等价变换，扩大/改善候选空间
2. 再用 Implementation 规则把逻辑算子转换为多个物理实现
3. 通过代价模型选择赢家（winner plan）

因此：

- **TopN 规则属于“Transformation（逻辑→逻辑）”**
- **TopN 算子（物理执行）属于“Implementation（逻辑→物理）”**

### 1.3 Cascade 是什么？

Cascade 是一种经典的查询优化器框架（在数据库系统里很常见），它把“优化”拆成两件事，并用统一的机制把它们组织起来：

1. **等价变换（Transformation）**：把一个逻辑计划改写成若干个语义等价的逻辑计划
2. **物理实现（Implementation）**：把逻辑算子映射到具体可执行的物理算子，并在多个候选里挑一个“赢家”（winner）

你可以把 Cascade 理解为：

- 用 **Memo** 存所有“等价表达式”（避免重复优化）
- 用 **Rule + Pattern** 自动发现“还能怎么改写/怎么实现”
- 用 **代价模型（Cost Model）** 在候选物理计划里选最优

### 1.4 为什么需要 Cascade 框架？

在真实系统里，单靠“手写若干 if/else 的优化流程”会遇到三个常见问题：

- **计划空间爆炸**：规则一多、组合一多，候选计划数量会指数增长；需要一个机制“去重 + 缓存 + 复用”。
- **RBO 与 CBO 很难揉在一起**：既要做逻辑等价变换（RBO），又要比较物理代价（CBO），还要控制先后顺序。
- **可扩展性差**：每加一个新规则/新算子都改主流程，长期会变成“牵一发动全身”。

Cascade 的价值就在于：

- 把“主流程”固定下来（探索等价、实现物理、选择赢家）
- 把变化点留给“规则”与“代价模型”，便于同学们在实验里增量实现 TopN/聚合下推

### 1.5 Cascade 框架的每一部分在做什么（结合本项目代码）

下面按“组件 → 作用 → 在 MiniOB 里大致在哪里”来理解；你在实现规则时会频繁用到这些概念。

#### 1.5.1 `Memo`：等价计划的缓存与去重

- **作用**：存储所有已经发现的等价表达式，避免相同子计划被反复优化；也让规则反复作用时不会无限生成重复结构。
- **直观理解**：Memo 是一张“等价类图谱”，不是一棵树。
- **代码落点**：`src/observer/sql/optimizer/cascade/memo.h/.cpp`

#### 1.5.2 `Group`：一个语义等价类

- **作用**：一个 Group 表示“一组语义等价的表达式”，例如同一个子查询可能既有 `OrderBy+Limit` 的形态，也可能有 `TopN` 的形态。
- **代码落点**：`src/observer/sql/optimizer/cascade/group.h/.cpp`

#### 1.5.3 `GroupExpr`：Memo 里的一个表达式节点

- **作用**：记录“算子 + 子 group 引用（child group ids）”。
- **关键点**：它不直接挂真实 child 节点，而是指向 child 的 group id；这就是 Memo 能表达 DAG/图结构的原因。
- **代码落点**：`src/observer/sql/optimizer/cascade/group_expr.h/.cpp`

#### 1.5.4 `Pattern`：规则匹配的形状描述

- **作用**：描述一个规则想匹配的计划形状，例如 TopN 规则要匹配 `Limit(OrderBy(X))`。
- **代码落点**：`src/observer/sql/optimizer/cascade/pattern.h`

#### 1.5.5 `Rule`：把“如何改写/如何实现”模块化

在本项目里你会看到两大类规则：

- **Transformation Rules（逻辑→逻辑）**
  - 作用：做等价变换，扩大/改善候选逻辑空间
  - TopN 就是典型：`Limit(OrderBy(X)) -> TopN(X)`
  - 代码落点：`src/observer/sql/optimizer/cascade/transformation_rules.h/.cpp`

- **Implementation Rules（逻辑→物理）**
  - 作用：把某个逻辑算子变成一种物理算子实现
  - TopN 的实现规则：`LogicalTopN -> TopNPhysicalOperator`
  - 代码落点：`src/observer/sql/optimizer/cascade/implementation_rules.h/.cpp`

规则本身需要被“注册”进优化器的 RuleSet：

- **规则注册**：`src/observer/sql/optimizer/cascade/rules.h/.cpp`

#### 1.5.6 `Promise`：控制规则优先级

- **作用**：当多个规则都能应用时，用 Promise（优先级）控制“先做哪些规则”。
- **实践经验**：一般先做 transformation（让逻辑形态更好），再做 implementation（落到物理）。
- **代码落点**：通常在各个 rule 类里定义/返回 promise（具体以骨架实现为准）。

#### 1.5.7 代价模型（Cost Model）与 Winner 选择

- **作用**：对不同物理候选计算代价（cost），选择最便宜的作为 winner plan。
- **代码落点**：`src/observer/sql/optimizer/cascade/cost_model.h/.cpp`

> 你实现 TopN 后，哪怕不改代价模型，通常也能通过“规则改写”让计划形态更优（例如少一次全量排序）。

#### 1.5.8 优化器入口与整体流程（你调试时看哪里）

- **作用**：把初始逻辑树放进 Memo，然后不断应用规则，最终产出一棵物理执行计划。
- **代码落点**：`src/observer/sql/optimizer/cascade/optimizer.h/.cpp`
- **辅助**：优化过程里可能会有 task/队列等调度结构（用于分阶段推进）：`src/observer/sql/optimizer/cascade/tasks/`、`pending_tasks.h`

> 实验实现的 TopN 需要你同时补齐“逻辑算子 + 物理算子 + 两类规则 + 注册”，然后用 EXPLAIN 观察 winner plan 是否出现 `TOPN`。

---

## 2. 实验任务拆分

- **任务 1（本文内容）：TopN**
  - 目标：把 `Limit(OrderBy(X))` 识别为 TopN，避免全量排序
  - 产物：TopN 逻辑算子、TopN 物理算子、TopN transformation rule、TopN implementation rule、测试与 EXPLAIN 验证

- **任务 2：聚合下推**
  - 目标：把“标量聚合 + inner join”的部分工作下推到 join 一侧，减少 join 后需要处理的数据量
  - 产物：一条 transformation rule（逻辑→逻辑），以及必要的表达式/算子细节补齐与测试

---

## 3. 任务 1：实现 TopN 算子与规则

### 3.1 优化作用与应用场景

#### 3.1.1 为什么需要 TopN？

`ORDER BY` 的典型实现需要对输入进行全量排序：

- 输入规模为 $N$，排序复杂度通常是 $O(N \log N)$

而当 SQL 形如：

```sql
SELECT ... FROM ... ORDER BY k LIMIT n;
```

我们只需要“前 n 个最小/最大”，可以使用**TopN** 思想：

- 维护一个大小为 $n$ 的堆（或选择算法）
- 扫描全表时只保留最优的 n 条
- 复杂度约为 $O(N \log n)$（当 $n \ll N$ 时收益显著）

#### 3.1.2 适用场景

- 排行榜/Top-K：`ORDER BY score DESC LIMIT 10`
- 分页第一页：`ORDER BY id LIMIT 20`
- 任何 `ORDER BY + LIMIT` 且 `LIMIT` 较小的查询

> 实验先实现最基础版本：`Limit(OrderBy(X)) -> TopN(X)`。

---

### 3.2 你需要实现的功能点（从“规则”到“可执行”）

实现 TopN 不是只加一个算子那么简单，而是要打通：

1. **Transformation（逻辑改写）**：识别 `Limit(OrderBy(child))` 并改写为 `TopN(child)`
2. **Implementation（物理实现）**：把 `LogicalTopN` 实现成 `TopNPhysicalOperator`
3. **算子定义**：新增逻辑/物理算子类型、打印名字（用于 EXPLAIN）
4. **测试验证**：确保 EXPLAIN 中出现 `TOPN`，并且结果正确

---

### 3.3 建议的实现步骤（按文件分解）

> 下列路径为本项目常见组织方式；若骨架代码路径略有差异，以你仓库为准。

#### Step A：添加算子类型枚举（让系统“认识”TopN）

- 文件：`src/observer/sql/operator/operator_node.h`
  - 在 `OpType` 中加入：
    - `LOGICALTOPN`（逻辑算子类型）
    - `TOPN`（物理算子类型）

- 文件：`src/observer/sql/operator/logical_operator.cpp`
  - 在 `logical_operator_type_name` 中加入 `LOGICALTOPN -> "LOGICAL_TOPN"`

- 文件：`src/observer/sql/operator/physical_operator.cpp`
  - 在 `physical_operator_type_name` 中加入 `TOPN -> "TOPN"`

**为什么要做这一步？**

- EXPLAIN/调试输出依赖这些 name
- Rule Pattern 也会依赖 `OpType`

#### Step B：实现 TopN 逻辑算子（LogicalTopN）

- 新增文件（或在对应目录新增类）：
  - `src/observer/sql/operator/logical/topn_logical_operator.h`

建议类职责：

- 存储：
  - `order_by_exprs`（排序键表达式列表）
  - `asc/desc`（每个键的升降序）
  - `limit`（TopN 的 N）
- 支持：
  - `clone()`：用于 memo/规则生成新节点
  - `hash()/operator==`：用于 memo 去重/等价判断

#### Step C：实现 TopN 物理算子（TopNPhysicalOperator）

- 新增文件：
  - `src/observer/sql/operator/physical/topn_physical_operator.h`
  - `src/observer/sql/operator/physical/topn_physical_operator.cpp`

建议实现策略（最简单可用版）：

- `open()`：
  - 打开 child
  - 迭代 child 的 tuple
  - 计算排序键（对每个 `order_by_expr` 调 `get_value()`）
  - 维护一个大小为 `N` 的堆（或直接收集后排序；但那就不是 TopN 了）
  - 最终得到已排序的 TopN 结果集（例如把堆内元素排序后保存在 `rows_`）
- `next()/current_tuple()`：
  - 依次输出 `rows_`
- `close()`：
  - 清理缓存，关闭 child

> 注意：同学们需要保证 `ORDER BY ... LIMIT` 的结果顺序正确。若存在相等键（tie），可以先按输入顺序保持稳定（stable），或按 RID（若可得）做稳定 tie-break。

#### Step D：实现 Transformation Rule：`Limit(OrderBy(X)) -> TopN(X)`

- 文件：`src/observer/sql/optimizer/cascade/transformation_rules.h/.cpp`

规则形态（示意）：

- `match_pattern`：
  - root：`LOGICALLIMIT`
  - child：`LOGICALORDERBY`
  - grandchild：`LEAF`

`transform()` 逻辑要点：

1. 读取 limit 值（若 limit < 0 可直接不改写）
2. 找到 order by 表达式与 asc/desc
3. 构造 `TopNLogicalOperator(order_exprs, asc, limit)`
4. 输出一个候选表达式：TopN 的 child 直接指向 OrderBy 的 child group id（跳过 OrderBy）

**为什么这是等价变换？**

- `ORDER BY K LIMIT N` 的语义就是输出前 N 个最小/最大
- TopN 是更高效的实现方式

#### Step E：实现 Implementation Rule：`LogicalTopN -> TopNPhysicalOperator`

- 文件：`src/observer/sql/optimizer/cascade/implementation_rules.h/.cpp`

`match_pattern`：`LOGICALTOPN(LEAF)`

`transform()`：

- 从 `TopNLogicalOperator` 拿到 `order_exprs/asc/limit`
- 深拷贝表达式（`expr->copy()`），创建 `TopNPhysicalOperator`
- 输出候选物理表达式，并保持 child group id 不变

> 建议：物理算子执行期可能比优化器 memo 生命周期更长，因此物理算子中用到的表达式尽量“自持有”（deep copy + unique_ptr 持有），避免悬空指针。

#### Step F：注册规则（让优化器真的会用）

- 文件：`src/observer/sql/optimizer/cascade/rules.cpp`

把规则加入 RuleSet：

- Transformation 规则集合：加入 TopNRule
- Implementation 规则集合：加入 LogicalTopNToTopN

并确保排序/优先级合理（一般：逻辑规则先于物理规则）。

---

### 3.4 如何验证你实现正确

#### 3.4.1 用 EXPLAIN 验证计划形态

准备一条有排序与 limit 的 SQL，例如：

```sql
EXPLAIN SELECT * FROM t ORDER BY id DESC LIMIT 10;
```

你应该观察到：

- 物理计划里出现 `TOPN`
- 且没有出现 `ORDER_BY` + `LIMIT` 的组合（或至少 TopN 替代了它们）

#### 3.4.2 用结果正确性验证

同一条查询：

```sql
SELECT * FROM t ORDER BY id DESC LIMIT 10;
```

- 输出条数必须为 10（或不足 10 则全部输出）
- 顺序必须满足 `ORDER BY` 指定的排序

建议覆盖：

- `ASC/DESC`
- 多 key 排序：`ORDER BY a, b DESC LIMIT n`
- 存在重复键（tie）的情况

#### 3.4.3 用测试用例自动化验证（推荐）

你可以在测试框架中添加一个新 case，例如：

- `test/case/test/primary-topn.test`
- `test/case/result/primary-topn.result`

并在 `.test` 中加入：

```sql
-- ensure:topn explain select ... order by ... limit ...;
select ... order by ... limit ...;
```

如果测试框架支持 `ensure:*` 指令，可以新增一个 ensure：

- `ensure:topn`：检查 EXPLAIN 文本中包含一次 `TOPN`

（实现位置通常在 `test/case/miniob_test.py` 的 ensure 分支中。）

---

## 4. 提交物（任务 1 完成标准）

- 代码能编译
- `EXPLAIN` 对 `ORDER BY ... LIMIT n` 展示 `TOPN`
- 查询结果与排序/limit 语义一致
- 至少 1 个自动化测试用例通过（推荐包含 `ensure:topn`）

---

## 5. 任务 2：聚合下推（标量聚合下推到 Join 一侧）

### 5.1 优化原理：为什么能下推？（核心等价变换）

本任务聚焦一种非常常见的形态：

```sql
SELECT  Agg(expr)
FROM    L INNER JOIN R ON L.k = R.k
;
```

其中 `Agg` 是标量聚合（**不带 `GROUP BY`**），例如 `COUNT(*)`、`SUM(L.v)`、`MIN(L.v)`。

关键观察：

- `INNER JOIN` 会把一侧的行“按匹配数复制（duplication）”
- 如果最终聚合只引用 **一张表**（例如只用到 `L.v`），那么“复制次数”只取决于**另一侧**在相同 join key 下有多少行

这里**不要求** `k` 是 `R`（或 `L`）的主键/唯一键：

- 即使 `R.k` 有重复值，只要我们对 `R` 按 `k` 做分组得到 `cnt = COUNT(1)`，这个 `cnt` 仍然精确表示“每个 `L` 行会被复制多少次”。
- 例如：`R` 里 `k=1` 有 3 行，那么任何 `L.k=1` 的行在 join 后都会出现 3 次；所以 `SUM(L.v)` 的等价改写就是对这些 `L.k=1` 的行做 `v * 3`。

因此我们可以先在“会导致复制的一侧”上做一个局部统计：

1. 在 **被复制次数决定的一侧**（例如 R）上按 join key 分组，算出每个 key 的行数：

  $$R' = \gamma_{k;\;\text{cnt}=COUNT(1)}(R)$$

  这里的 `COUNT(1)` **不是“把值都改成 1”**，而是“对每个分组统计行数”：
  - `1` 只是一个常量表达式，`COUNT(1)` 在每一行都会产生 1，因此 `COUNT(1)` 的结果等价于“该分组里有多少行”
  - 所以关键在于 **按 join key（可能是多列）分组**：每个 key 被压缩成一行，`cnt` 记录原来该 key 有多少条记录

2. 再把原始的 L 和这个 `R'` join 起来（R 的大表被压缩成“每个 key 一行”）：

  > 这里的“每个 key 一行”是指 **分组后的 `R'`** 每个 key 只保留一行（key 对应的计数/聚合结果），并不是要求原表 `R` 的 key 天然唯一。

   $$J' = L \bowtie_{L.k=R'.k} R'$$

3. 最后把顶层聚合改写为使用 `cnt` 来修正复制：

- `COUNT(*)`：原本统计的是 join 后的总行数；等价于把每个 key 的复制次数加起来
  - 改写：`COUNT(*) -> SUM(cnt)`
- `SUM(L.v)`：join 后每个 `L.v` 会重复 `cnt` 次
  - 改写：`SUM(L.v) -> SUM(L.v * cnt)`
- `MIN(L.v)` / `MAX(L.v)`：重复不会改变最小/最大
  - 改写：保持 `MIN(L.v)` / `MAX(L.v)` 不变

这就是本次实验实现的“聚合下推到一侧”的本质：

- **下推的不是最终聚合本身**，而是先在 join 的一侧算一个“复制次数（join count）”
- 用这个复制次数把顶层聚合改写成等价形式

### 5.2 本次实验的实现边界（必须满足/不处理的情况）

为了把任务控制在可实现且可验证的范围内，本实验只要求支持下面的子集。

#### 5.2.1 必须满足的前提（满足才触发规则）

- 顶层必须是 `LOGICALGROUPBY`，且是**标量聚合**：`group_by_expressions` 为空
- 顶层聚合表达式列表非空，且每个都是 `AggregateExpr`
- 子节点必须是 `LOGICALINNERJOIN(L, R)`
- `L` 与 `R` 都是“单表输入”（只包含一个 `LOGICALGET` 的 group；不含再 join/聚合/子查询等）
- Join 条件仅支持：
  - 由 `AND` 连接的若干个等值条件
  - 每个等值条件形如 `L.k = R.k`（左右都是 `FieldExpr`，且能明确表名）
- **不要求** join key 是主键/唯一键（重复 key 会通过 `COUNT(1)` 计入 `__CBO_JOIN_CNT`）
- 顶层聚合仅引用**单侧**：
  - 要么只引用 L 的字段/表达式
  - 要么只引用 R 的字段/表达式
  - 对 `COUNT(*)` / `COUNT(1)` 这种“不引用字段”的聚合，可以任选一侧作为“被聚合侧”

#### 5.2.2 本次不要求处理（遇到直接不改写）

- 顶层带 `GROUP BY`（非标量聚合）
- `AVG()`（因为需要同时处理 sum/count，改写更复杂）
- `COUNT(col)` 且 `col` 引用表字段（涉及 NULL 语义；本实验不展开）
- 聚合表达式同时引用两侧（例如 `SUM(L.a + R.b)`）
- 非等值 join / 含 OR 的 join 条件 / 复杂 join 谓词
- join 的任一侧不是单表（例如 join 上面再有 selection/project/another join）

> 备注：这些边界并不是“理论上不可能”，只是为了让同学们把精力聚焦在 Cascade 的 Rule + Pattern + Memo 改写流程上。

### 5.3 你需要实现的功能点（从规则到计划形态）

你要实现的是一个 **Transformation Rule（逻辑→逻辑）**：

```
ScalarGroupBy( InnerJoin(L, R) )
  ->
ScalarGroupBy'( InnerJoin( L, HashGroupBy(R) ) )
```

其中：

- `HashGroupBy(R)`：按 join key 分组，产出每个 key 的 `__CBO_JOIN_CNT = COUNT(1)`
- `ScalarGroupBy'`：把原聚合列表改写成等价形式（`COUNT(*) -> SUM(__CBO_JOIN_CNT)` 等）

### 5.4 建议的实现步骤（按文件/函数分解）

> 下列路径以当前 MiniOB 代码组织为准；若骨架代码有差异，以实际仓库为准。

#### Step A：添加规则类型并注册

- 文件：`src/observer/sql/optimizer/cascade/rules.h`
  - 在 `RuleType` 加入：`AGGREGATE_JOIN_PUSHDOWN`

- 文件：`src/observer/sql/optimizer/cascade/rules.cpp`
  - 在 transformation rule 集合里注册你的新规则（确保 promise/顺序合理）

#### Step B：实现 Transformation Rule 类

- 文件：`src/observer/sql/optimizer/cascade/transformation_rules.h/.cpp`
  - 新增类：`AggregateJoinPushdownRule`
  - `match_pattern` 建议写成：
    - root：`LOGICALGROUPBY`
    - child：`LOGICALINNERJOIN(LEAF, LEAF)`

`transform()` 的关键步骤（建议按这个顺序做）：

1. **校验边界**：
   - 顶层 group by 是否为空（标量聚合）
   - join 是否为 inner join、左右 child 是否存在
2. **解析 join key**：
   - 只接受 `AND` 的等值条件
   - 收集 join key 列表（统一成“左表 key / 右表 key”的方向）
3. **判断聚合引用在哪一侧**：
   - 遍历每个 `AggregateExpr` 的 child，收集其中引用到的表名
   - 若聚合只引用 L：则在 R 上做局部 group by
   - 若聚合只引用 R：则在 L 上做局部 group by
   - 若 `COUNT(*)`：两侧都行（任选一侧作为“被聚合侧”，另一侧做局部 group by）
4. **构造局部 GroupBy（产生 join count）**：
   - `group_by_expressions = join keys（来自被压缩的一侧）`
   - `aggregate_expressions = [ COUNT(1) AS __CBO_JOIN_CNT ]`
5. **构造新的 Join**：
   - 让 join 的一侧替换为“局部 group by 的输出”
   - join predicate 保持不变
6. **改写顶层聚合列表**（标量 group by，不带 group keys）：
   - `COUNT(*)`（或 `COUNT(1)`）改写为：`SUM(__CBO_JOIN_CNT)`
   - `SUM(x)` 改写为：`SUM(x * __CBO_JOIN_CNT)`
   - `MIN(x)` / `MAX(x)` 保持不变
   - 保持聚合表达式的 `name()`（否则 SELECT 列别名/投影取值可能失败）

#### Step C：表达式复制与生命周期（避免悬空指针）

在 Cascade 中，规则改写会频繁 `copy()/clone()` 表达式与算子。建议确保：

- `AggregateExpr::copy()` 会把 `name()`（以及必要的位置信息）一起复制
  - 文件：`src/observer/sql/expr/expression.h`

如果你在执行期遇到悬空指针（例如物理算子持有的表达式来自 memo/logical 的临时对象），需要让算子“自持有”表达式：

- 逻辑/物理 `GroupBy` 里用 `unique_ptr<Expression>` 持有聚合表达式，并提供 raw 指针视图给执行逻辑
  - 参考落点：
    - `src/observer/sql/operator/logical/group_by_logical_operator.h/.cpp`
    - `src/observer/sql/operator/physical/group_by_physical_operator.h/.cpp`

> 这部分属于“工程性收尾”，不是聚合下推理论本身，但非常常见；做对了能显著减少调试成本。

### 5.5 如何验证你实现正确

#### 5.5.1 用 EXPLAIN 看计划形态

准备一条“标量聚合 + inner join”的 SQL，例如：

```sql
EXPLAIN SELECT COUNT(*), SUM(L.v)
FROM L INNER JOIN R ON L.k = R.k;
```

触发规则后，你应当能看到计划里出现一个“按 join key 分组”的 group by（通常会实现成 `HASH_GROUP_BY`），并且顶层仍是标量聚合（通常是 `SCALAR_GROUP_BY`）。

#### 5.5.2 用结果校验语义等价

至少覆盖：

- `COUNT(*)`
- `SUM(L.v)`（聚合引用单侧）
- `MIN/MAX`（可选）

可以用小数据集手算核对。

#### 5.5.3 用测试用例自动化验证（推荐）

- 新增 case：`test/case/test/primary-aggregate-pushdown.test`
- 新增结果：`test/case/result/primary-aggregate-pushdown.result`
- 在 `.test` 里增加 ensure：

```sql
-- ensure:hashgroupby explain select count(*), sum(L.v) from ... join ...;
select count(*), sum(L.v) from ... join ...;
```

若测试框架没有 `ensure:hashgroupby`，可在 `test/case/miniob_test.py` 增加一个 ensure 分支：检查 EXPLAIN 输出包含一次 `HASH_GROUP_BY`。

---

## 6. 常见问题（FAQ）

### Q1：为什么我实现了 TopN 但 EXPLAIN 仍然是 ORDER_BY + LIMIT？

可能原因：

- Transformation rule 没注册/没匹配到 pattern
- Rule promise/执行顺序导致实现规则先走了
- 改写后没有正确替换 child group id（TopN 的 child 应该是 OrderBy 的 child）

### Q2：如何快速判断规则是否触发？

- 打开 optimizer 的 TRACE/DEBUG 日志（若骨架提供）
- 在 rule 的 `transform()` 中临时打印（实验阶段可用）
- 通过 EXPLAIN 是否出现 `TOPN` 判断
