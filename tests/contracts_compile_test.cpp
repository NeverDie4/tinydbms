#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "tinydbms/common.hpp"
#include "tinydbms/compiler.hpp"
#include "tinydbms/core.hpp"
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

static_assert(std::is_move_constructible_v<core::Database>);
static_assert(!std::is_copy_constructible_v<core::Database>);

// 公开 API 必须存在且签名正确；这里只做类型检查，不产生链接依赖。
static_assert(std::is_same_v<
    decltype(split_statements(std::declval<std::string_view>())),
    std::vector<SplitStatement>>);
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

}  // namespace

int main() {
    using namespace tinydbms;
    using namespace tinydbms::compiler;

    // 构造一棵可编译的最小查询树，验证递归类型与移动语义可用。
    auto lhs = std::make_unique<Expr>(ColumnRef{0});
    auto rhs = std::make_unique<Expr>(Literal{Value{std::int32_t{1}}});
    Expr predicate{Binary{CmpOp::kEq, std::move(lhs), std::move(rhs)}};

    PlanNode scan{SeqScanNode{0}};
    FilterNode filter{std::move(predicate), std::make_unique<PlanNode>(std::move(scan))};
    PlanNode filtered{std::move(filter)};
    ProjectNode project{std::vector<ColumnId>{0}, std::make_unique<PlanNode>(std::move(filtered))};
    PlanNode projected{std::move(project)};
    QueryPlan query{std::make_unique<PlanNode>(std::move(projected))};

    Plan plan{std::move(query)};
    CompileResult ok{std::move(plan)};
    CompileResult err{CompileError{CompileErrorKind::kSyntax, SourceLocation{1, 1}, "syntax error"}};

    storage::Record record{storage::RecordId{1}, std::vector<Value>{}};
    core::ExecuteResult result;
    result.outcome = core::CommandResult{0, std::nullopt};

    (void)ok;
    (void)err;
    (void)record;
    (void)result;
    return 0;
}
