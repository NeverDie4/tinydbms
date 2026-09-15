#ifndef TINYDBMS_CORE_ANALYSIS_CATALOG_HPP
#define TINYDBMS_CORE_ANALYSIS_CATALOG_HPP

#include "tinydbms/compiler.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace tinydbms::core::internal {

enum class ShadowApplyResult {
    kNoCatalogChange,  // INSERT/SELECT/DELETE 等不改变 Catalog 的 Plan
    kApplied,          // CREATE TABLE 成功写入影子 Catalog
    kRejected          // CREATE TABLE 结构化失败（ID 耗尽、保留 ID、重复名称、非法元数据）
};

// 脚本局部影子 Catalog：只复制运行时状态，不调用 Storage，不写磁盘。
struct AnalysisCatalog {
    std::vector<TableMeta> tables;
    std::uint64_t next_table_id = 0;
};

// 复制运行时 Catalog 与 64 位计数器；分配失败会抛出，调用方按致命中止处理。
AnalysisCatalog make_analysis_catalog(
    const std::vector<TableMeta>& runtime_tables,
    std::uint64_t runtime_next_table_id);

// 用运行态已确认的元数据同步影子副本；抛出时影子状态保持不变。
void mirror_runtime_catalog(
    AnalysisCatalog& analysis,
    const std::vector<TableMeta>& runtime_tables,
    std::uint64_t runtime_next_table_id);

struct ShadowApplyOutcome {
    ShadowApplyResult result = ShadowApplyResult::kNoCatalogChange;
    std::string rejection_message;  // 仅 kRejected 使用
};

// 分析态执行入口：第一轮只模拟 CREATE TABLE。
ShadowApplyOutcome apply_supported_ddl(
    AnalysisCatalog& analysis,
    const compiler::Plan& plan);

}  // namespace tinydbms::core::internal

#endif  // TINYDBMS_CORE_ANALYSIS_CATALOG_HPP
