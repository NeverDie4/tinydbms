# tinydbms SQL v2 扩展契约

本文冻结 SQL v2 在 compiler、core、storage、app 之间共享的设计方向。它描述的是后续分阶段落地的目标契约，不表示当前代码已经实现 SQL v2。当前可执行依据仍是 `include/tinydbms/*.hpp` 与测试；每个迁移阶段完成后，必须同步更新公共头文件、实现、测试和现有 SQL v1 文档。

本文只扩展 SQL v1，不重写 SQL v1 已验证的生命周期、错误分类、UTF-8 字节列、资源上限和模块所有权规则。若本文与 SQL v1 文档在本页明确列出的扩展点冲突，以本文作为 SQL v2 的目标设计。

## 1. 当前 v1 基线与模块边界

当前实现的关键限制如下：

| 主题 | SQL v1 当前状态 | SQL v2 方向 | 主要受影响模块 |
|---|---|---|---|
| 类型 | `INT`、`VARCHAR` | 增加 `BIGINT`、`DOUBLE`、`BOOLEAN` | common/compiler/core/storage/app/tests |
| 值 | `variant<int32_t, string>` | 增加 NULL、int64、double、bool | common/compiler/core/storage/app/tests |
| 列元数据 | name + type | 增加 nullable | common/compiler/core/storage/tests |
| 表达式列引用 | `ColumnId` | query-local `SlotId` | compiler/core/tests |
| 查询算子 | Scan/Filter/Project | 增加 Join/Aggregate/Sort | compiler/core/tests |
| 语句 | CREATE/INSERT/SELECT/DELETE | 增加 UPDATE | compiler/core/storage/app/tests |
| 诊断 | kind/location/message | 增加 suggestion/FixIt/SourceRange | common/compiler/core/app/tests |
| 脚本 | 首错立即停止 | 首错后继续分析、永久停止执行 | core/app/tests |
| 存储格式 | `TINYDBMS_STORAGE_V1` | 可读 V1，新增 V2 格式 | storage/core/tests |

职责边界保持不变：compiler 只编译一条语句；core 负责脚本编排、Catalog、表达式执行和 Storage 调用；storage 不理解 SQL、表达式、SlotId、JOIN 或 GROUP BY；app 只负责输入和展示。

## 2. Type、Value 与数值字面量

### 2.1 公共类型

SQL v2 的公共类型集合冻结为：

```cpp
enum class Type {
    kInt,
    kBigInt,
    kDouble,
    kBoolean,
    kVarchar
};
```

| Type | SQL 名称 | C++ 值 |
|---|---|---|
| `kInt` | `INT` | `std::int32_t` |
| `kBigInt` | `BIGINT` | `std::int64_t` |
| `kDouble` | `DOUBLE` | `double` |
| `kBoolean` | `BOOLEAN` | `bool` |
| `kVarchar` | `VARCHAR` | `std::string` |

不增加 `Type::kNull`。NULL 是值状态而不是列类型；DATE、DECIMAL 等若未来加入，属于 SQL v3，不提前加入占位枚举。

### 2.2 公共 Value

```cpp
struct Value {
    std::variant<
        std::monostate,
        std::int32_t,
        std::int64_t,
        double,
        bool,
        std::string
    > data;
};
```

`std::monostate` 唯一表示 SQL NULL。NULL 不携带自身 Type，其目标类型由列 schema 或 semantic context 决定；禁止 sentinel integer、magic string 和 `Type::kNull`。

选择 monostate 而非 `Value { Type, optional<Scalar> }` 的原因是：现有 Value 已以 variant 保证非 NULL 值的唯一运行时类型，加入一个空 alternative 即可保持单一判别源；后者会同时维护 Type 与 Scalar alternative，产生不一致状态，并扩大所有构造、比较和存储边界。需要 NULL 的目标类型时，应从已绑定表达式或 schema 取得，而不是复制进 Value。

### 2.3 整数与 DOUBLE 字面量

SQL v2 第一阶段只接受非负数值字面量，不增加 unary minus。因此 `-1` 和 `-3.14` 不属于 SQL v2 第一阶段语法，负数留给独立后续扩展。

整数按数值范围推断：

| 文本范围 | 字面量类型/结果 |
|---|---|
| `0..2147483647` | `INT` |
| `2147483648..9223372036854775807` | `BIGINT` |
| 大于 `9223372036854775807` | `CompileErrorKind::kLex` |

例如 `1` 和 `2147483647` 是 INT，`2147483648` 和 `9223372036854775807` 是 BIGINT，`9223372036854775808` 是 lexical literal overflow。超出 INT64 不形成合法 numeric token/value，因此不是 Semantic Error。

DOUBLE 的首版词法形式严格为 `digits '.' digits`，例如 `0.0`、`1.0`、`12.5`。不支持 `.5`、`1.`、指数形式、NaN、Infinity 或 `-inf`；转换结果不是 finite 时返回 kLex。Lexer 同时提供独立 `kDot`：数字两侧均满足规则时形成 floating literal，`identifier '.' identifier` 则形成 qualified reference，不混用同一 token。

### 2.4 数值 widening 与比较 common type

仅支持以下 widening：

```text
INT -> BIGINT -> DOUBLE
```

允许 INT→BIGINT、INT→DOUBLE、BIGINT→DOUBLE；禁止所有自动 narrowing。BOOLEAN 和 VARCHAR 不参与 numeric promotion。BIGINT→DOUBLE 服从 IEEE-754 binary64，可能发生精度损失；这是显式冻结的比较/赋值语义。

数值比较先求 common type：INT/BIGINT 得 BIGINT，任一侧 DOUBLE 得 DOUBLE。VARCHAR 和 BOOLEAN 首版只允许 `=`、`!=`；BOOLEAN 不支持顺序比较。VARCHAR/BOOLEAN 与 numeric 之间没有隐式转换。INSERT 和 UPDATE 的字面量可按同一 widening 规则转成目标列 Value；运行时比较由 Core 对 Value alternatives 做同样的确定性 widening。

## 3. BOOLEAN、TruthValue 与三值逻辑

SQL BOOLEAN 是可存储的 `Value{bool}`。谓词真假是 Core/Optimizer 内部逻辑概念：

```cpp
enum class TruthValue {
    kFalse,
    kTrue,
    kUnknown
};
```

表达式求值统一返回 Value，不再长期保留当前 `variant<Value, bool>` 的双 bool 表示。谓词入口把 BOOLEAN/NULL Value 解释为 TruthValue：true→kTrue，false→kFalse，NULL→kUnknown。由此 `WHERE active` 合法；只保留 kTrue 行，kFalse 与 kUnknown 均被过滤。

TruthValue 不是公共 SQL Type，也不进入 Storage。compiler 内部类型仍可把“可作谓词的 BOOLEAN”作为语义类型处理，但生成的 BOOLEAN literal/column 在运行时统一是 Value。

### 3.1 Truth table

| X | NOT X |
|---|---|
| TRUE | FALSE |
| FALSE | TRUE |
| UNKNOWN | UNKNOWN |

| AND | TRUE | FALSE | UNKNOWN |
|---|---|---|---|
| TRUE | TRUE | FALSE | UNKNOWN |
| FALSE | FALSE | FALSE | FALSE |
| UNKNOWN | UNKNOWN | FALSE | UNKNOWN |

| OR | TRUE | FALSE | UNKNOWN |
|---|---|---|---|
| TRUE | TRUE | TRUE | TRUE |
| FALSE | TRUE | FALSE | UNKNOWN |
| UNKNOWN | TRUE | UNKNOWN | UNKNOWN |

普通比较只要任一操作数为 NULL，结果就是 UNKNOWN，包括 `NULL = NULL` 与 `NULL != NULL`。

### 3.2 Optimizer metadata

Optimizer 的 `optional<bool> constant_bool` 在 NULL 阶段迁移为 `optional<TruthValue>`：

- `nullopt`：不是编译期常量；
- kTrue/kFalse/kUnknown：分别是已证明的常量三值结果；
- UNKNOWN 与 nullopt 不可混同。

现有 TRUE AND X、FALSE AND X、FALSE OR X、TRUE OR X、Double-NOT 规则在三值逻辑下仍安全。ConstantComparison 必须能产生 UNKNOWN。statement root 只有 kTrue 可以 reset predicate；kFalse 和 kUnknown 必须保留。Rule identity、registry 与 trace 不变，只扩展规则内部 metadata 语义。

## 4. NULL 与 nullable

### 4.1 ColumnMeta

```cpp
struct ColumnMeta {
    std::string name;
    Type type;
    bool nullable = false;
};
```

trailing default `false` 只用于保持旧 C++ 聚合初始化 `ColumnMeta{name, type}` 的源代码与 SQL v1 非空语义兼容。SQL v2 CREATE 的 parser/semantic 必须显式填值：

- 未写约束：`nullable=true`；
- `NULL`：`nullable=true`；
- `NOT NULL`：`nullable=false`。

因此“C++ 省略字段默认 false”与“SQL 省略 NULL 约束默认 true”是两个不同入口，不能互相推断。

Phase 2 CREATE 支持 `column_name type [NULL | NOT NULL]`，不支持 PRIMARY KEY、UNIQUE、DEFAULT、CHECK 或 FOREIGN KEY。

### 4.2 NULL literal 与 null test

NULL 可出现在 INSERT VALUES、UPDATE SET literal、comparison operand、`IS NULL` 和 `IS NOT NULL`。无类型 NULL 由上下文绑定；写入 NOT NULL 列属于 Semantic Error。`x = NULL` 语法和语义合法但结果 UNKNOWN，可附带 `did you mean 'IS NULL'?` hint。

为避免把 IS NULL 伪装成普通 comparison，公共/Bound Expr 增加专用 null-test 形态：

```cpp
enum class NullTestOp { kIsNull, kIsNotNull };

struct NullTest {
    NullTestOp op;
    std::unique_ptr<Expr> operand;
};
```

IS NULL 对任意已绑定标量类型合法，永远返回非 NULL BOOLEAN；IS NOT NULL 是其逻辑反值。

## 5. SourceRange 与高级诊断

```cpp
struct SourceRange {
    SourceLocation begin;
    SourceLocation end;
};

struct FixIt {
    SourceRange range;
    std::string replacement;
};

struct CompileError {
    CompileErrorKind kind;
    SourceLocation location;
    std::string message;
    std::optional<std::string> suggestion = std::nullopt;
    std::optional<FixIt> fix_it = std::nullopt;
};
```

SourceRange 使用 half-open `[begin, end)`，begin/end 都是 1-based line 与 1-based UTF-8 byte column；零长度插入满足 begin==end。`SELEC` 位于行首时，其 range 为 `[1:1, 1:6)`，replacement 为 `SELECT`。CompileError 的两个 trailing optional 带默认值，尽量保持旧三字段 aggregate initialization 可编译；首版不增加 warning severity、多 FixIt、notes 或 error code 系统。

### 5.1 did-you-mean

- keyword 候选仅来自 Parser 当前语法上下文中的 expected tokens；
- table 候选来自当前 CatalogView；
- column 候选来自当前 Semantic relation scope；
- 比较使用大小写不敏感的 normalized identifier 与 bounded Damerau-Levenshtein；
- 输入长度 1..4：distance≤1；5..8：distance≤2；>8：distance≤min(3, floor(length/3))；
- 只有阈值内唯一最佳候选才提示，并列最佳不提示。

Phase 8 首批高置信度 FixIt 是 keyword typo 替换、缺分号插入、缺逗号插入和缺右括号插入。table/column suggestion 可只有文字；range 明确时允许附 replacement FixIt，不强制每条 suggestion 都有 FixIt。

## 6. TableId、ColumnId 与 query-local SlotId

```cpp
using SlotId = std::uint32_t;
```

三类 ID 的职责严格分离：

| ID | 作用域 | 含义 | 是否持久化/传给 Storage |
|---|---|---|---|
| TableId | Database Catalog | 表身份 | 是 |
| ColumnId | 单个 TableMeta | schema 列序 0..n-1 | 是 |
| SlotId | 单棵 Query/Update Plan | 绑定后的数据流槽位 | 否 |

SlotId 从 0 确定性递增，不依赖指针地址，不能直接拿 ColumnId 数值代替。FROM/JOIN relation 按出现顺序，每张表按 TableMeta column 顺序分配原始 slots；aggregate 派生值继续从 next slot 分配。例如 student 三列为 0..2，随后 course 三列为 3..5。

当前 storage 私有 slotted-page slot 不是 query SlotId；实现时必须保持命名空间限定，二者不共享语义。

### 6.1 Scan binding 与 ColumnRef

```cpp
struct ScanColumn {
    ColumnId column_id;
    SlotId output_slot;
};

struct SeqScanNode {
    TableId table_id;
    std::vector<ScanColumn> columns;
};

struct ColumnRef {
    SlotId slot_id;
};
```

SeqScan 首版可以输出表的所有列，但原表列到 slot 的映射必须显式存在。所有 public Expr 的 ColumnRef 统一引用 SlotId；UPDATE predicate 也不例外。

Core 内部执行行必须能区分“slot 未由当前节点产生”和“slot 已产生且值为 SQL NULL”。建议使用按 SlotId 索引的 presence + Value frame（例如 `vector<optional<Value>>`）；不可单用 monostate 表示缺失槽位，因为 monostate 已表示真实 NULL。该执行 frame 是 Core 私有实现，不进入 Storage 或查询结果公共 Row。

## 7. QueryPlan v2 与数据流

```cpp
struct QueryOutput {
    SlotId slot_id;
    std::string name;
    Type type;
    bool nullable;
};

struct QueryPlan {
    std::unique_ptr<PlanNode> root;
    std::vector<QueryOutput> outputs;
};

struct ProjectNode {
    std::vector<SlotId> outputs;
    std::unique_ptr<PlanNode> child;
};
```

Project 只选择/重排上游 slots，允许重复并保留顺序。QueryPlan.outputs 与 SELECT item 一一对应，为 Core/App 提供最终名字、类型和 nullable；普通列名使用原列名，aggregate 名使用 `COUNT(*)`、`AVG(score)` 等稳定文本，首版无 AS alias。JOIN 后 Core 不再依赖单表 Catalog 反查结果 schema。

PlanNode v2 包含 SeqScan、Filter、Join、Aggregate、Sort、Project。典型逻辑顺序如下：

```text
Project
└── Sort                 // 有 ORDER BY 时
    └── Aggregate        // 有 aggregate/GROUP BY 时
        └── Filter       // WHERE
            └── Join     // 或 SeqScan
                ├── ...
                └── ...
```

省略未出现的节点。WHERE 在聚合前；Sort 在 Project 前，因此未投影但在该阶段仍可见的 slot 可以作为排序键。无 aggregate 查询为 `Project -> Sort -> Filter -> Join/Scan`。

### 7.1 ORDER BY

```cpp
enum class SortDirection { kAsc, kDesc };

struct SortKey {
    SlotId slot_id;
    SortDirection direction;
};

struct SortNode {
    std::vector<SortKey> keys;
    std::unique_ptr<PlanNode> child;
};
```

语法直接支持多个 key，省略方向默认为 ASC。`SELECT name FROM student ORDER BY age;` 合法并生成 `Project(name) -> Sort(age) -> ...`。可排序类型为 INT/BIGINT/DOUBLE/BOOLEAN/VARCHAR；BOOLEAN 为 false<true，VARCHAR 使用 UTF-8 raw-byte/binary collation。NULL 无论 ASC/DESC 均固定 NULLS LAST；首版不接受显式 NULLS FIRST/LAST 语法。

### 7.2 INNER JOIN 与 qualified reference

`JOIN` 与 `INNER JOIN` 等价；不支持 alias、LEFT/RIGHT/FULL/CROSS。支持 `table.column`。未限定列在整个 relation scope 唯一时合法，存在多个同名候选时返回 ambiguous column Semantic Error。

```cpp
enum class JoinKind { kInner };

struct JoinNode {
    JoinKind kind;
    Expr condition;
    std::unique_ptr<PlanNode> left;
    std::unique_ptr<PlanNode> right;
};
```

condition 的 ColumnRef 均为 SlotId。多 JOIN 首版按 SQL relation 顺序形成确定性的 left-deep tree；ON 可引用当前累计左侧与当前右侧 relation。Core 使用 Nested Loop Join，condition 为 TRUE 才匹配，FALSE/UNKNOWN 不匹配；Storage 只提供各表扫描，不感知 JOIN。

## 8. Aggregate 与 GROUP BY

Aggregate 不作为普通逐行 Expr 交给现有 evaluator，而使用独立节点：

```cpp
enum class AggregateKind { kCount, kSum, kAvg, kMin, kMax };

struct AggregateCall {
    AggregateKind kind;
    std::optional<SlotId> input_slot;  // COUNT(*) 为 nullopt
    SlotId output_slot;
    Type output_type;
    bool nullable;
};

struct AggregateNode {
    std::vector<SlotId> group_keys;
    std::vector<AggregateCall> aggregates;
    std::unique_ptr<PlanNode> child;
};
```

首版 aggregate 参数只接受 `*`（仅 COUNT）或 column_ref，不接受一般 scalar expression。Aggregate 输出 group-key slots 和新分配的 aggregate slots，供上层 Sort/Project 引用。例如 group key Slot2、COUNT(*) Slot3，Project outputs 为 `[2,3]`。

### 8.1 类型矩阵

| 函数 | 输入 | 输出 | NULL/空输入 |
|---|---|---|---|
| COUNT(*) | 任意行 | BIGINT, nullable=false | 每行计数；空输入 0 |
| COUNT(expr) | 任意 Type | BIGINT, nullable=false | 忽略 NULL；空输入 0 |
| SUM | INT | BIGINT, nullable=true | 忽略 NULL；无非 NULL 输入为 NULL |
| SUM | BIGINT | BIGINT, nullable=true | 同上；溢出作为 execution error，不回绕 |
| SUM | DOUBLE | DOUBLE, nullable=true | 同上；结果必须遵循 finite 值策略 |
| AVG | INT/BIGINT/DOUBLE | DOUBLE, nullable=true | 忽略 NULL；无非 NULL 输入为 NULL |
| MIN/MAX | INT/BIGINT/DOUBLE/VARCHAR | 输入 Type, nullable=true | 忽略 NULL；无非 NULL 输入为 NULL |

SUM/AVG 的 BOOLEAN/VARCHAR（AVG 也包括 VARCHAR）为 Semantic Error；MIN/MAX BOOLEAN 首版不允许。DOUBLE 聚合若产生非 finite 结果返回 execution error，不产生可存储的非 finite Value。

### 8.2 GROUP BY semantic

首版 SELECT item 只允许普通 ColumnRef 或 AggregateCall。查询含 aggregate 或 GROUP BY 时，每个非 aggregate SELECT column 必须出现在 GROUP BY keys；否则返回 `column is neither grouped nor aggregated` Semantic Error。`SELECT COUNT(*) FROM student;` 无 GROUP BY 时，整个输入是一个 group。无 HAVING、arithmetic、scalar function 或 alias。

## 9. UPDATE 契约

首版语法只允许单表、literal assignment：

```text
UPDATE table
SET column = literal {, column = literal}
[WHERE bool_expr]
;
```

```cpp
struct UpdateAssignment {
    ColumnId column_id;
    Value value;
};

struct UpdatePlan {
    TableId table_id;
    std::vector<ScanColumn> input_columns;
    std::vector<UpdateAssignment> assignments;
    std::optional<Expr> predicate;  // ColumnRef 使用 SlotId
};
```

SET target 是 schema ColumnId；predicate 统一使用 input_columns 建立的 SlotId。重复 SET column 是 Semantic Error。无 WHERE 允许全表 UPDATE。assignment 按 SQL 顺序保存但目标不得重复；字面量必须与目标列同型或可 widening，NULL 仅可写 nullable 列。不支持 column expression、算术、JOIN UPDATE 或 subquery。

Core 先 open/scan/evaluate，构造完整 replacement rows 并收集 RID；成功 close cursor 后才调用 Storage update。Storage public API 采用批量完整行替换，概念为：

```cpp
struct UpdateRow { RecordId rid; std::vector<Value> values; };
struct UpdateRequest { TableId table_id; std::vector<UpdateRow> rows; };
struct UpdateResult {
    std::uint64_t updated_count;
    std::optional<StorageError> error;
};
```

Storage 可 in-place 或 relocate；RecordId 稳定性不是 SQL contract。按请求顺序执行，partial failure 用 updated_count + error 表达。Core 不得在 Storage API 外拼接 delete + insert，以免破坏失败语义和记录所有权。

## 10. Storage V2 格式与 V1 兼容

### 10.1 当前 V1 byte-level 概要

当前 metadata 首行为 `TINYDBMS_STORAGE_V1`，以 TABLE/COLUMN/ENDTABLE/END 文本行保存 schema；类型 token 为 INT32 或 VARCHAR。V1 record 没有 record header、type tag 或 null bitmap，按 schema 顺序连接：INT 为 4-byte signed little-endian，VARCHAR 为 4-byte unsigned little-endian byte length 后接 UTF-8 bytes。

### 10.2 版本选择与 metadata

V2 code 必须读取既有 V1 database。打开 V1 时，所有列映射为 SQL v2 ColumnMeta 且 `nullable=false`，现有 table files 继续使用 V1 decoder。新 database 直接写 `TINYDBMS_STORAGE_V2`，新建 SQL v2 table 使用 V2 row format；不自动原地重写 V1 records。

为了让同一 database 在升级后同时容纳旧 V1 表和新 V2 表，V2 metadata 冻结为 per-table row format：

```text
TINYDBMS_STORAGE_V2
TABLE <table_id> <table_name> <V1|V2>
COLUMN <INT32|INT64|DOUBLE|BOOLEAN|VARCHAR> <NOT_NULL|NULLABLE> <column_name>
...
ENDTABLE
END
```

标识符不含空白，因此无需新 quoting。首次需要持久化 V2 schema 时，可只把 metadata 重写为 V2：旧表标记 V1、新表标记 V2；旧 table files 不迁移。V1 表仅允许其既有 INT/VARCHAR、NOT NULL schema 和 V1-compatible 写入。row-format 是 Storage 私有表描述的一部分，不加入 public TableMeta。

### 10.3 V2 row codec

一条 V2 record payload：

```text
[null bitmap: ceil(column_count / 8) bytes]
[每个 non-NULL column 的 payload，按 schema 顺序紧密排列]
```

bitmap bit i 位于 byte `i/8` 的 bit `i%8`，1 表示 NULL、0 表示有 payload；末字节未使用高位必须为 0。NULL 列不编码 payload；若 non-nullable 列对应 bit 为 1，encode 返回 invalid request，decode 返回 corrupt。NULL VARCHAR 因 bitmap 为 1 且没有长度/payload，与非 NULL 空字符串的 length=0 明确区分。

非 NULL payload：

| Type | 编码 |
|---|---|
| INT | 4-byte signed little-endian |
| BIGINT | 8-byte signed little-endian |
| DOUBLE | IEEE-754 binary64 bit pattern，8-byte little-endian |
| BOOLEAN | 1 byte：0=false、1=true；其他值为 corrupt |
| VARCHAR | uint32 little-endian byte length + UTF-8 bytes |

DOUBLE 必须通过稳定的 bit representation 与显式 little-endian 编解码，不依赖 host-endian raw reinterpret_cast。SQL/Storage 写入拒绝非 finite double；decode 遇非 finite payload 视为 corrupt。

`kMaxVarcharBytes` 继续按 UTF-8 bytes 限制。`kMaxRowLogicalBytes` 的 V2 逻辑载荷为所有 non-NULL 值之和：INT 4、BIGINT 8、DOUBLE 8、BOOLEAN 1、VARCHAR 为字符串 bytes，NULL 为 0；bitmap 与 VARCHAR length prefix 不计逻辑载荷，但完整 encoded payload 仍必须满足单页物理上限。

## 11. ScriptResult 与错误恢复

Script Recovery 属于 Core public API，不进入 compiler。为复用现有 ExecuteResult 并补齐逐语句状态，目标结构为：

```cpp
enum class StatementStatus {
    kExecuted,
    kCompileError,
    kExecutionError,
    kAnalysisOnly
};

struct StatementResult {
    std::size_t statement_index;       // 0-based，按 split 顺序
    SourceRange source_range;          // 整段 script 的绝对范围
    StatementStatus status;
    std::optional<ExecuteResult> outcome;
};

struct ExecuteScriptResult {
    std::vector<StatementResult> statements;
    std::optional<std::size_t> first_error_index;
    std::size_t executed_count;
    std::optional<Error> script_error; // splitter 无法产生可靠 statement 时
};
```

kExecuted 携带成功 QueryResult/CommandResult；kCompileError 携带转换后的 core Error；kExecutionError 携带 Error 或带 error 的 partial CommandResult；kAnalysisOnly 不执行且 outcome=nullopt。`first_error_index` 是 0-based statement_index。`executed_count` 表示实际进入 Executor 的语句数，包含最终以 kExecutionError 结束的那条，不包含 compile error 或 analysis-only。script_error 只承载整段分句失败，此时可能没有 statement_index。

这是对当前 `vector<ExecuteResult> outcomes` 的有意 breaking change，但复用 ExecuteResult，避免再造结果 variant。迁移期可在 Core/App 内提供 legacy outcomes 投影视图，不把重复状态长期保存在公共结构。

### 11.1 fail-stop execution policy

execute_script 初始 `execution_enabled=true`。首个 Compile、Execution 或 Storage Error 将其永久改为 false；本次调用内绝不恢复。此前成功语句保留副作用，不 rollback，本功能不是 transaction。

首错之后仍对每个可可靠分出的 statement 运行 Lexer→Parser→Semantic→Optimizer→Planner，但绝不调用 Executor 或 Storage。后续编译成功为 kAnalysisOnly，后续编译失败仍记录 kCompileError。首错可能是 execution/storage error；同样从下一条开始进入 analysis-only 模式。

## 12. Analysis / Shadow Catalog

Shadow Catalog 由单次 `Database::execute_script()` 局部拥有，调用结束即丢弃。首错前 compiler 使用实时 Runtime Catalog，DDL 成功执行后照常更新它。首错发生时复制此刻 Runtime Catalog 与 next_table_id，后续 compile 只借用 Shadow Catalog。

analysis-only CREATE 编译成功后，按正常 deterministic ID allocation 模拟加入 Shadow Catalog；INSERT、UPDATE、DELETE、SELECT 不改变 schema。连续 analysis-only CREATE 因而能被后续 SELECT/JOIN/GROUP BY 看见，重复 CREATE 也会得到准确 duplicate-table Semantic Error。Shadow Catalog 只复制 metadata，不访问 Storage。

Shadow TableId 从 Runtime `next_table_id` 副本开始，按相同规则递增；不使用 UINT32_MAX sentinel、负数或特殊高位，因为 TableId 全范围合法。这些 ID 只服务本次 analysis，Plan 不执行。ID 耗尽时记录 analysis diagnostic。

## 13. 位置换算与恢复边界

CompileError.location 和 FixIt.range 在 compile 返回时均相对单条 statement。Core 转成 script 绝对坐标：

```text
absolute.line = statement.start.line + local.line - 1

local.line == 1:
absolute.column = statement.start.column + local.column - 1

local.line > 1:
absolute.column = local.column
```

SourceRange 的 begin/end 分别使用同一公式。规则继续是 1-based、UTF-8 byte column，CRLF 视作一个换行且 `\r` 不占列。StatementResult.source_range 为 `[SplitStatement.start, advance(start, sql))`。

Statement-level recovery 依赖 splitter 能可靠识别 `;` 边界。普通语法/语义错误（如 `SELECT id,,name FROM t;`）不会阻止后续 statement 分析；但未闭合 string 或 block comment 可能让后续分号处于无法判定的 lexical construct 中。首版不承诺从这种输入恢复后续 statement，也不做 heuristic resynchronization 或 same-statement panic recovery。

## 14. SQL v2 grammar 范围

以下是冻结范围的概略 grammar；细节 token/AST 可分阶段实现，但不能扩大语义：

```text
statement      := create | insert | select | delete | update

create         := CREATE TABLE identifier '('
                  column_def {',' column_def} ')' ';'
column_def     := identifier type [NULL | NOT NULL]
type           := INT | BIGINT | DOUBLE | BOOLEAN | VARCHAR

insert         := INSERT INTO identifier
                  ['(' identifier {',' identifier} ')']
                  VALUES value_row {',' value_row} ';'
value_row      := '(' literal {',' literal} ')'

select         := SELECT select_item {',' select_item}
                  FROM relation
                  { [INNER] JOIN relation ON bool_expr }
                  [WHERE bool_expr]
                  [GROUP BY column_ref {',' column_ref}]
                  [ORDER BY order_key {',' order_key}]
                  ';'
select_item    := column_ref | aggregate_call | '*'
aggregate_call:= COUNT '(' ('*' | column_ref) ')'
                | (SUM | AVG | MIN | MAX) '(' column_ref ')'
relation       := identifier
order_key      := column_ref [ASC | DESC]

delete         := DELETE FROM identifier [WHERE bool_expr] ';'
update         := UPDATE identifier SET assignment {',' assignment}
                  [WHERE bool_expr] ';'
assignment     := identifier '=' literal

column_ref     := identifier | identifier '.' identifier
literal        := integer | double | string | TRUE | FALSE | NULL
bool_expr      := comparison | null_test | boolean_column
                  | NOT bool_expr
                  | '(' bool_expr ')'
                  | bool_expr AND bool_expr
                  | bool_expr OR bool_expr
comparison     := scalar_operand comparison_op scalar_operand
null_test      := scalar_operand IS [NOT] NULL
```

SQL v2 新增 keyword 的完整冻结列表：

```text
BIGINT DOUBLE BOOLEAN TRUE FALSE NULL IS
UPDATE SET
ORDER BY ASC DESC
JOIN INNER ON
GROUP
COUNT SUM AVG MIN MAX
```

NOT 已存在；BY 同时服务 ORDER BY 与 GROUP BY。不得提前加入 LEFT、RIGHT、FULL、OUTER、CROSS、HAVING、DISTINCT、AS 或 LIMIT。

## 15. Breaking Public Contract Changes

| 结构/格式 | v1 | v2 | 原因 | Compiler | Core | Storage | App |
|---|---|---|---|---|---|---|---|
| Type | INT/VARCHAR | +BIGINT/DOUBLE/BOOLEAN | SQL v2 scalar types | High | High | High | Medium |
| Value | int32/string | monostate/int32/int64/double/bool/string | NULL 与新值类型 | High | High | High | Medium |
| ColumnMeta | name/type | +nullable | NULL schema | High | High | High | Low |
| SourceRange | 无 | begin/end half-open | FixIt/statement range | Medium | Medium | None | Medium |
| CompileError | 3 fields | +optional suggestion/FixIt | diagnostics | High | Medium | None | Medium |
| ColumnRef | ColumnId | SlotId | JOIN/aggregate 唯一绑定 | High | High | None | None |
| SeqScanNode | table_id | table_id + ScanColumn mapping | 建立物理列→slot | High | High | None | None |
| ProjectNode | vector<ColumnId> | vector<SlotId> | 统一数据流 | High | High | None | None |
| QueryPlan | root | root + QueryOutput metadata | JOIN/aggregate 结果 schema | High | High | None | Medium |
| PlanNode | Scan/Filter/Project | +Join/Aggregate/Sort | 新查询能力 | High | High | None | None |
| Plan | Create/Insert/Delete/Query | +Update | UPDATE | High | High | Medium | Low |
| Core expression result | variant<Value,bool> | Value + internal TruthValue | 消除双 bool、支持 UNKNOWN | Medium | High | None | None |
| Storage metadata | STORAGE_V1 | STORAGE_V2 + per-table row format | 混合读 V1/V2 | None | Low | High | None |
| Storage record | 无 null bitmap | bitmap + typed payload | NULL/新类型 | None | Medium | High | None |
| Storage API | insert/scan/delete | +update | UPDATE partial result | None | High | High | Low |
| ExecuteScriptResult | vector outcomes | per-statement status/range + summary | recovery/analysis-only | None | High | None | High |

High 项必须先完成公共 contract tests，再逐模块迁移；在所有消费者迁移前不得让中间提交伪装成可联调完成态。

## 16. 实施顺序

后续只按小步迁移，不在一次改动中落地全部结构：

1. Phase 0C — Public Contract Skeleton：Type/Value/nullable/SourceRange/SlotId 等基础结构及 contract tests。
2. Phase 1A — BIGINT Lexer/Parser。
3. Phase 1B — BIGINT Semantic/Core/Storage。
4. Phase 1C — DOUBLE 全链路。
5. Phase 1D — BOOLEAN + TruthValue refactor。
6. Phase 2A — NULL Value/Column contract。
7. Phase 2B — Storage V2 NULL codec 与 V1 compatibility。
8. Phase 2C — IS NULL/IS NOT NULL + 3VL + Optimizer metadata。
9. Phase 3 — UPDATE + Storage update API。
10. Phase 4 — ORDER BY/SortNode。
11. Phase 5 — INNER JOIN、qualified reference 与 Slot execution。
12. Phase 6 — Aggregate + GROUP BY。
13. Phase 7 — did-you-mean。
14. Phase 8 — FixIt。
15. Phase 9 — Script Recovery result/policy。
16. Phase 10 — Analysis/Shadow Catalog。
17. Phase 11 — compiler/core/storage/app integration。
18. Phase 12 — fuzz、V1/V2 persistence 与 execution equivalence。

每阶段保持 g++ strict warnings、已有 CTest 和 public behavior 回归；Phase 0C 只迁移骨架，不顺带实现 SQL 语法。

## 17. 明确排除范围

SQL v2 本阶段不包含：negative numeric literal、arithmetic、DATE/TIME、DECIMAL、CHAR/TEXT/BLOB、LEFT/RIGHT/FULL/CROSS JOIN、table/column alias、HAVING、DISTINCT、LIMIT/OFFSET、subquery、UNION、PRIMARY KEY、FOREIGN KEY、UNIQUE、DEFAULT、CHECK、transaction、rollback、same-statement panic recovery 或 cost-based optimizer。

## 18. 一致性检查与已知迁移冲突

以下结论用于防止实现阶段出现双重语义：

1. NULL 只在 Value 中用 monostate 表示，Type 没有 kNull；ColumnMeta.nullable 决定能否存储 NULL。
2. SQL BOOLEAN 是 Value{bool}，TruthValue 只服务 predicate/optimizer，二者不重复存储。
3. TableId/ColumnId 绑定持久对象，SlotId 绑定单棵计划；Storage 永远看不到 SlotId。
4. Aggregate output 获得新 SlotId，Project 不再假设所有输出都来自原表 ColumnId。
5. Sort 位于 Project 下方、数据源上方；有聚合时位于 Aggregate 上方，确保排序 slot 尚可见。
6. V2 metadata 的 per-table format 允许 V1 decoder 与 V2 decoder 共存，无需原地重写旧 records。
7. Runtime Catalog 只反映已执行 DDL；首错后 Shadow Catalog 只服务分析，绝不触发 Storage。
8. Optimizer 的 UNKNOWN 是已知常量而非 unknown-analysis；只有常量 TRUE 能删除 root predicate。
9. CompileError.location 保持单点主位置；FixIt 使用 local SourceRange，Core 统一转换为 script 绝对范围。
10. script 首错后继续 compiler 全流水线但永久停止 executor；首错前副作用不回滚。

当前真实实现与冻结目标存在两项必须在迁移测试中显式处理的差异：

- 当前 Lexer/测试已接受 signed INT32 literal（包括 `-2147483648`），而 SQL v2 本契约冻结第一阶段 numeric literal 为非负。Phase 0C/1A 必须先锁定 v2 测试，再移除或隔离旧 signed-literal 行为；这不是 unary arithmetic 的延续。
- 当前正整数 `2147483648` 被归为超出 INT32 的 Semantic Error；SQL v2 中它变为合法 BIGINT，而仅超出 INT64 才是 kLex。错误阶段与边界测试必须随 BIGINT 迁移，不可沿用 v1 断言。

除此之外，本文选择的 NULL/TruthValue、SlotId/aggregate、Sort placement、Storage versioning、Shadow Catalog 与 diagnostics 结构之间未发现无法同时满足的设计冲突。
