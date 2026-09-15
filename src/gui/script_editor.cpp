#include "script_editor.hpp"

#include <QColor>
#include <QList>
#include <QTextCharFormat>
#include <QTextBlock>
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
    connect(this, &QPlainTextEdit::textChanged, this, [this] {
        has_executable_text_.reset();
        emit text_edited();
    });
}

void ScriptEditor::clear_diagnostics() {
    setExtraSelections(QList<QTextEdit::ExtraSelection>{});
}

void ScriptEditor::apply_diagnostics(const std::vector<DiagnosticSpan>& spans) {
    const QString text = toPlainText();
    std::vector<tinydbms::SourceRange> ranges;
    ranges.reserve(spans.size());
    for (const DiagnosticSpan& span : spans) {
        ranges.push_back(span.range);
    }
    const std::vector<std::optional<EditorRange>> mapped = editor_ranges(text, ranges);

    QList<QTextEdit::ExtraSelection> selections;
    for (std::size_t index = 0; index < spans.size(); ++index) {
        if (!mapped[index].has_value()) {
            continue;
        }
        selections.append(make_selection(document(), *mapped[index], spans[index].error));
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

bool ScriptEditor::has_executable_text() const {
    if (!has_executable_text_.has_value()) {
        has_executable_text_ = compute_executable_text();
    }
    return *has_executable_text_;
}

bool ScriptEditor::compute_executable_text() const {
    // 等价于 !toPlainText().trimmed().isEmpty()，但不复制整篇文档。
    for (QTextBlock block = document()->begin(); block.isValid(); block = block.next()) {
        for (const QChar character : block.text()) {
            if (!character.isSpace()) {
                return true;
            }
        }
    }
    return false;
}

}  // namespace tinydbms::gui
