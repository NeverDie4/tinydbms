#ifndef TINYDBMS_GUI_SCRIPT_EDITOR_HPP
#define TINYDBMS_GUI_SCRIPT_EDITOR_HPP

#include <optional>
#include <vector>

#include <QPlainTextEdit>

#include "tinydbms/diagnostic.hpp"

namespace tinydbms::gui {

// 诊断高亮的最小单位：错误范围标红，语句范围标灰。
struct DiagnosticSpan {
    tinydbms::SourceRange range;
    bool error = false;
};

class ScriptEditor : public QPlainTextEdit {
    Q_OBJECT

public:
    explicit ScriptEditor(QWidget* parent = nullptr);

    void clear_diagnostics();
    // 越界或无效范围被忽略，不影响其余高亮。
    void apply_diagnostics(const std::vector<DiagnosticSpan>& spans);
    // 用 QTextCursor 在换算后的位置替换文本；范围无效时返回 false。
    bool apply_fix(const tinydbms::FixIt& fix);

    // 是否有可执行内容（至少一个非空白字符）。结果在文本变化时失效、按需重算：
    // 调用方（按钮状态、执行入口）不必为每次判断复制整篇文档。
    bool has_executable_text() const;

signals:
    void text_edited();

private:
    bool compute_executable_text() const;

    mutable std::optional<bool> has_executable_text_;
};

}  // namespace tinydbms::gui

#endif  // TINYDBMS_GUI_SCRIPT_EDITOR_HPP
