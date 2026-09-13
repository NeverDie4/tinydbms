#include "script_editor.hpp"

#include <QColor>
#include <QList>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>

#include <optional>

#include "source_mapping.hpp"

namespace tinydbms::gui {
namespace {

QTextEdit::ExtraSelection make_selection(
    QTextDocument* document,
    const EditorRange& range,
    bool error) {
    QTextCursor cursor{document};
    cursor.setPosition(range.begin);
    if (range.end > range.begin) {
        cursor.setPosition(range.end, QTextCursor::KeepAnchor);
    } else {
        // 空范围（插入点）无法用零宽选区显示：向后扩一个字符，
        // 位于文档末尾时向前扩一个字符，保证用户能看到标记。
        if (range.begin < document->characterCount() - 1) {
            cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
        } else if (range.begin > 0) {
            cursor.setPosition(range.begin - 1);
            cursor.setPosition(range.begin, QTextCursor::KeepAnchor);
        }
    }

    QTextEdit::ExtraSelection selection;
    selection.cursor = cursor;
    selection.format.setBackground(
        error ? QColor{255, 205, 205} : QColor{232, 232, 232});
    if (error) {
        selection.format.setUnderlineStyle(QTextCharFormat::WaveUnderline);
        selection.format.setUnderlineColor(QColor{190, 40, 40});
    }
    return selection;
}

}  // namespace

ScriptEditor::ScriptEditor(QWidget* parent) : QPlainTextEdit{parent} {
    setLineWrapMode(QPlainTextEdit::NoWrap);
    connect(this, &QPlainTextEdit::textChanged, this, &ScriptEditor::text_edited);
}

void ScriptEditor::clear_diagnostics() {
    setExtraSelections(QList<QTextEdit::ExtraSelection>{});
}

void ScriptEditor::apply_diagnostics(const std::vector<DiagnosticSpan>& spans) {
    const QString text = toPlainText();
    QList<QTextEdit::ExtraSelection> selections;
    for (const DiagnosticSpan& span : spans) {
        const std::optional<EditorRange> range = editor_range(text, span.range);
        if (!range.has_value()) {
            continue;
        }
        selections.append(make_selection(document(), *range, span.error));
    }
    setExtraSelections(selections);
}

bool ScriptEditor::apply_fix(const tinydbms::FixIt& fix) {
    const std::optional<EditorRange> range = editor_range(toPlainText(), fix.range);
    if (!range.has_value()) {
        return false;
    }
    QTextCursor cursor{document()};
    cursor.setPosition(range->begin);
    cursor.setPosition(range->end, QTextCursor::KeepAnchor);
    cursor.insertText(QString::fromUtf8(fix.replacement));
    return true;
}

}  // namespace tinydbms::gui
