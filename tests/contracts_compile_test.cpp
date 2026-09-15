#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "tinydbms/common.hpp"
#include "tinydbms/compiler.hpp"
#include "tinydbms/core.hpp"
#include "tinydbms/diagnostic.hpp"
#include "tinydbms/storage.hpp"

namespace {

using namespace tinydbms;
using namespace tinydbms::compiler;

static_assert(!std::is_default_constructible_v<Expr>);
static_assert(!std::is_copy_constructible_v<Expr>);
static_assert(std::is_move_constructible_v<Expr>);

static_assert(!std::is_default_constructible_v<PlanNode>);
static_assert(!std::is_default_constructible_v<QueryPlan>);
static_assert(!std::is_default_constructible_v<Plan>);
static_assert(!std::is_default_constructible_v<CompileResult>);
static_assert(!std::is_default_constructible_v<SplitStatementsResult>);
static_assert(std::is_move_constructible_v<SplitStatementsResult>);
static_assert(!std::is_copy_constructible_v<SplitStatementsResult>);

static_assert(std::is_move_constructible_v<core::Database>);
static_assert(!std::is_copy_constructible_v<core::Database>);
static_assert(std::is_same_v<SlotId, std::uint32_t>);
static_assert(std::is_same_v<decltype(ColumnRef::slot_id), SlotId>);
static_assert(std::is_same_v<decltype(ScanColumn::column_id), ColumnId>);
static_assert(std::is_same_v<decltype(ScanColumn::output_slot), SlotId>);
static_assert(std::is_same_v<decltype(ProjectNode::outputs), std::vector<SlotId>>);
static_assert(std::is_same_v<decltype(SortKey::slot_id), SlotId>);
static_assert(std::is_same_v<decltype(SortKey::direction), SortDirection>);
static_assert(std::is_same_v<decltype(SortNode::keys), std::vector<SortKey>>);
static_assert(std::is_same_v<decltype(JoinNode::kind), JoinKind>);
static_assert(std::is_same_v<decltype(JoinNode::condition), Expr>);
static_assert(std::is_same_v<decltype(AggregateCall::kind), AggregateKind>);
static_assert(std::is_same_v<decltype(AggregateCall::input_slot), std::optional<SlotId>>);
static_assert(std::is_same_v<decltype(AggregateCall::output_slot), SlotId>);
static_assert(std::is_same_v<decltype(AggregateCall::output_type), Type>);
static_assert(std::is_same_v<decltype(AggregateNode::group_keys), std::vector<SlotId>>);
static_assert(std::is_same_v<decltype(AggregateNode::aggregates), std::vector<AggregateCall>>);
static_assert(std::is_same_v<decltype(QueryOutput::slot_id), SlotId>);
static_assert(std::is_same_v<decltype(UpdateAssignment::column_id), ColumnId>);
static_assert(std::is_same_v<decltype(UpdateAssignment::value), Value>);
static_assert(std::is_same_v<decltype(FixIt::range), SourceRange>);
static_assert(std::is_same_v<decltype(FixIt::replacement), std::string>);
static_assert(std::is_same_v<decltype(CompileError::stage), CompileStage>);
static_assert(std::is_same_v<decltype(CompileError::source), SourceRange>);
static_assert(std::is_same_v<decltype(CompileError::suggestion), std::optional<std::string>>);
static_assert(std::is_same_v<decltype(CompileError::fix_it), std::optional<FixIt>>);
static_assert(std::is_same_v<decltype(SplitStatement::source), SourceRange>);

static_assert(static_cast<int>(Type::kInt) == 0);
static_assert(static_cast<int>(Type::kVarchar) == 1);

// 公开 API 必须存在且签名正确；这里只做类型检查，不产生链接依赖。
static_assert(std::is_same_v<
    decltype(split_statements(std::declval<std::string_view>())),
    SplitStatementsResult>);
static_assert(std::is_same_v<
    decltype(compile(std::declval<const CompileRequest&>())),
    CompileResult>);

static_assert(std::is_same_v<
    decltype(std::declval<core::Database&>().open(std::declval<const core::OpenDatabaseRequest&>())),
    core::OpenDatabaseResult>);
static_assert(std::is_same_v<
    decltype(std::declval<core::Database&>().close()),
    core::CloseDatabaseResult>);
static_assert(std::is_same_v<
    decltype(std::declval<core::Database&>().execute_script(std::declval<const core::ExecuteScriptRequest&>())),
    core::ExecuteScriptResult>);
// 诊断类型：半开范围与阶段枚举取代旧的 CompileErrorKind；偏移归属 SourceLocation 端点。
static_assert(std::is_same_v<decltype(SourceLocation::byte_offset), std::size_t>);
static_assert(std::is_same_v<decltype(SourceRange::begin), SourceLocation>);
static_assert(std::is_same_v<decltype(SourceRange::end), SourceLocation>);
static_assert(!std::is_enum_v<SourceRange>);
static_assert(std::is_enum_v<CompileStage>);

// 逐语句结果只能经命名工厂构造，禁止默认构造与聚合初始化。
static_assert(!std::is_default_constructible_v<core::StatementResult>);
static_assert(!std::is_aggregate_v<core::StatementResult>);
static_assert(std::is_same_v<
    decltype(core::StatementResult::executed(
        std::declval<std::size_t>(),
        std::declval<SourceRange>(),
        std::declval<core::ExecuteResult>())),
    core::StatementResult>);
static_assert(std::is_same_v<
    decltype(core::StatementResult::execution_error(
        std::declval<std::size_t>(),
        std::declval<SourceRange>(),
        std::declval<core::ExecuteResult>())),
    core::StatementResult>);
static_assert(std::is_same_v<
    decltype(core::StatementResult::skipped(
        std::declval<std::size_t>(),
        std::declval<SourceRange>())),
    core::StatementResult>);
static_assert(std::is_same_v<
    decltype(std::declval<const core::StatementResult&>().status()),
    core::StatementStatus>);
static_assert(std::is_same_v<
    decltype(std::declval<const core::StatementResult&>().outcome()),
    const std::optional<core::ExecuteResult>&>);
static_assert(std::is_same_v<
    std::remove_cvref_t<decltype(std::declval<const core::Error&>().source)>,
    std::optional<SourceRange>>);
static_assert(std::is_same_v<
    std::remove_cvref_t<decltype(std::declval<const core::Error&>().compile_stage)>,
    std::optional<CompileStage>>);
static_assert(core::kMaxStatementsPerScript == 4096);
static_assert(core::kMaxQueryRows == (std::size_t{1} << 18));
// U3 计划模式 / U4 上限 / U5 取消令牌的公共契约：字段类型固定，默认值在 main() 运行期校验
// （请求对象含 CancelToken，不再是字面类型，不能用于常量表达式）。
static_assert(std::is_same_v<
    std::remove_cvref_t<decltype(core::ExecuteScriptRequest{}.mode)>,
    core::ExecutionMode>);
static_assert(std::is_same_v<
    std::remove_cvref_t<decltype(core::ExecuteScriptRequest{}.max_query_rows)>,
    std::size_t>);
static_assert(std::is_same_v<
    std::remove_cvref_t<decltype(core::ExecuteScriptRequest{}.cancel)>,
    core::CancelToken>);
static_assert(std::is_same_v<
    decltype(core::StatementResult::cancelled(
        std::declval<std::size_t>(),
        std::declval<SourceRange>())),
    core::StatementResult>);
static_assert(std::is_same_v<
    decltype(core::StatementResult::plan_only(
        std::declval<std::size_t>(),
        std::declval<SourceRange>(),
        std::declval<core::QueryResult>())),
    core::StatementResult>);
static_assert(std::is_same_v<
    decltype(core::ExecuteScriptResult::statements),
    std::vector<core::StatementResult>>);
static_assert(std::is_same_v<
    decltype(core::ExecuteScriptResult::script_error),
    std::optional<core::Error>>);

static_assert(std::is_same_v<
    decltype(storage::open_storage(std::declval<const storage::OpenStorageRequest&>())),
    storage::OpenStorageResult>);
static_assert(std::is_same_v<
    decltype(storage::close_storage(std::declval<const storage::CloseStorageRequest&>())),
    storage::CloseStorageResult>);
static_assert(std::is_same_v<
    decltype(storage::list_tables(std::declval<const storage::ListTablesRequest&>())),
    storage::ListTablesResult>);
static_assert(std::is_same_v<
    decltype(storage::create_table(std::declval<const storage::CreateTableRequest&>())),
    storage::CreateTableResult>);
static_assert(std::is_same_v<
    decltype(storage::open_table(std::declval<const storage::OpenTableRequest&>())),
    storage::OpenTableResult>);
static_assert(std::is_same_v<
    decltype(storage::scan_next(std::declval<const storage::ScanNextRequest&>())),
    storage::ScanNextResult>);
static_assert(std::is_same_v<
    decltype(storage::close_cursor(std::declval<const storage::CloseCursorRequest&>())),
    storage::CloseCursorResult>);
static_assert(std::is_same_v<
    decltype(storage::insert(std::declval<const storage::InsertRequest&>())),
    storage::InsertResult>);
static_assert(std::is_same_v<
    decltype(storage::delete_records(std::declval<const storage::DeleteRequest&>())),
    storage::DeleteResult>);
static_assert(std::is_same_v<
    decltype(storage::update_rows(std::declval<const storage::UpdateRequest&>())),
    storage::UpdateResult>);

}  // namespace

int main() {
    using namespace tinydbms;
    using namespace tinydbms::compiler;

    // 构造一棵可编译的最小查询树，验证递归类型与移动语义可用。
    auto lhs = std::make_unique<Expr>(ColumnRef{SlotId{7}});
    auto rhs = std::make_unique<Expr>(Literal{Value{std::int32_t{1}}});
    Expr predicate{Binary{CmpOp::kEq, std::move(lhs), std::move(rhs)}};
    Expr null_test_expression{NullTest{
        NullTestOp::kIsNull,
        std::make_unique<Expr>(ColumnRef{SlotId{7}})}};

    const std::vector<ScanColumn> scan_columns{{ColumnId{0}, SlotId{7}}};
    PlanNode scan{SeqScanNode{TableId{0}, scan_columns}};
    FilterNode filter{std::move(predicate), std::make_unique<PlanNode>(std::move(scan))};
    PlanNode filtered{std::move(filter)};
    SortNode sort{
        std::vector<SortKey>{SortKey{SlotId{7}, SortDirection::kDesc}},
        std::make_unique<PlanNode>(std::move(filtered))};
    PlanNode sorted{std::move(sort)};
    ProjectNode project{
        std::vector<SlotId>{SlotId{7}},
        std::make_unique<PlanNode>(std::move(sorted))};
    PlanNode projected{std::move(project)};
    const std::vector<QueryOutput> query_outputs{
        QueryOutput{SlotId{7}, "id", Type::kInt, false}};
    QueryPlan query{std::make_unique<PlanNode>(std::move(projected)), query_outputs};

    PlanNode join_plan{JoinNode{
        JoinKind::kInner,
        Expr{Literal{Value{true}}},
        std::make_unique<PlanNode>(SeqScanNode{
            TableId{0}, std::vector<ScanColumn>{{ColumnId{0}, SlotId{0}}}}),
        std::make_unique<PlanNode>(SeqScanNode{
            TableId{1}, std::vector<ScanColumn>{{ColumnId{0}, SlotId{1}}}})}};
    PlanNode aggregate_plan{AggregateNode{
        std::vector<SlotId>{SlotId{0}},
        std::vector<AggregateCall>{AggregateCall{
            AggregateKind::kCount,
            std::nullopt,
            SlotId{2},
            Type::kBigInt,
            false}},
        std::make_unique<PlanNode>(SeqScanNode{
            TableId{0}, std::vector<ScanColumn>{{ColumnId{0}, SlotId{0}}}})}};

    const DeletePlan deletion{
        TableId{0},
        scan_columns,
        std::nullopt};
    const Value contract_int_value{std::int32_t{1}};
    const UpdatePlan update{
        TableId{0},
        scan_columns,
        std::vector<UpdateAssignment>{
            UpdateAssignment{ColumnId{0}, Value{std::int32_t{2}}}},
        std::nullopt};
    const storage::UpdateRequest update_request{
        TableId{0},
        std::vector<storage::UpdateRow>{
            storage::UpdateRow{storage::RecordId{1}, std::vector<Value>{contract_int_value}}}};

    Plan plan{std::move(query)};
    CompileResult ok{std::move(plan)};
    const CompileError plain_error{
        CompileStage::kSyntax,
        SourceRange{SourceLocation{1, 1, 0}, SourceLocation{1, 1, 0}},
        "syntax error"};
    const FixIt replacement_fix{
        SourceRange{SourceLocation{1, 1, 0}, SourceLocation{1, 6, 5}},
        "SELECT"};
    const FixIt insertion_fix{
        SourceRange{SourceLocation{1, 20, 19}, SourceLocation{1, 20, 19}},
        ";"};
    const FixIt deletion_fix{
        SourceRange{SourceLocation{1, 7, 6}, SourceLocation{1, 8, 7}},
        ""};
    const CompileError rich_error{
        CompileStage::kSyntax,
        SourceRange{SourceLocation{1, 1, 0}, SourceLocation{1, 6, 5}},
        "unexpected token",
        std::optional<std::string>{"did you mean SELECT?"},
        std::optional<FixIt>{replacement_fix}};
    CompileResult err{plain_error};

    storage::Record record{storage::RecordId{1}, std::vector<Value>{}};
    core::ExecuteResult result;
    result.outcome = core::CommandResult{0, std::nullopt};

    const Value null_value{std::monostate{}};
    const Value int_value{std::int32_t{1}};
    const Value bigint_value{std::int64_t{2}};
    const Value double_value{3.5};
    const Value boolean_value{true};
    const Value string_value{std::string{"value"}};
    const ColumnMeta legacy_column{"id", Type::kInt};
    const ColumnMeta nullable_column{"name", Type::kVarchar, true};
    const SourceRange range{SourceLocation{2, 3, 7}, SourceLocation{4, 5, 19}};

    // 请求默认值：执行模式、默认上限、从未请求取消的令牌。
    const core::ExecuteScriptRequest default_request;
    core::CancelToken cancellable;
    const bool request_defaults_ok =
        default_request.error_policy == core::ScriptErrorPolicy::kStopOnFirstError &&
        default_request.mode == core::ExecutionMode::kExecute &&
        default_request.max_query_rows == core::kMaxQueryRows &&
        !default_request.cancel.cancel_requested() &&
        !cancellable.cancel_requested();
    cancellable.request_cancel();
    const bool cancel_ok = cancellable.cancel_requested() &&
        !default_request.cancel.cancel_requested();

    const bool common_contract =
        Type::kBigInt != Type::kInt &&
        Type::kDouble != Type::kInt &&
        Type::kBoolean != Type::kInt &&
        std::get<NullTest>(null_test_expression.kind).op == NullTestOp::kIsNull &&
        std::holds_alternative<std::monostate>(null_value.data) &&
        std::holds_alternative<std::int32_t>(int_value.data) &&
        std::holds_alternative<std::int64_t>(bigint_value.data) &&
        std::holds_alternative<double>(double_value.data) &&
        std::holds_alternative<bool>(boolean_value.data) &&
        std::holds_alternative<std::string>(string_value.data) &&
        !legacy_column.nullable && nullable_column.nullable &&
        range.begin.line == 2 && range.begin.column == 3 &&
        range.end.line == 4 && range.end.column == 5 &&
        plain_error.stage == CompileStage::kSyntax &&
        plain_error.source.begin.byte_offset == 0 &&
        !plain_error.suggestion.has_value() &&
        !plain_error.fix_it.has_value() &&
        replacement_fix.range.begin.line == 1 &&
        replacement_fix.range.begin.column == 1 &&
        replacement_fix.range.end.line == 1 &&
        replacement_fix.range.end.column == 6 &&
        replacement_fix.replacement == "SELECT" &&
        insertion_fix.range.begin.line == insertion_fix.range.end.line &&
        insertion_fix.range.begin.column == insertion_fix.range.end.column &&
        insertion_fix.replacement == ";" &&
        deletion_fix.range.begin.line == deletion_fix.range.end.line &&
        deletion_fix.range.begin.column < deletion_fix.range.end.column &&
        deletion_fix.replacement.empty() &&
        rich_error.suggestion == std::optional<std::string>{"did you mean SELECT?"} &&
        rich_error.fix_it.has_value() &&
        rich_error.fix_it->replacement == "SELECT";

    (void)ok;
    (void)err;
    (void)record;
    (void)result;
    (void)join_plan;
    (void)aggregate_plan;
    (void)deletion;
    (void)update;
    (void)update_request;
    return (common_contract && request_defaults_ok && cancel_ok) ? 0 : 1;
}
