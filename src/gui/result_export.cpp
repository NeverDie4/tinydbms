#include "result_export.hpp"

#include <QChar>
#include <QStringList>

#include <cstddef>

#include "result_model.hpp"

namespace tinydbms::gui {
namespace {

QString escape_field(const QString& text, QChar separator) {
    const bool needs_quotes = text.contains(separator) || text.contains(QLatin1Char('"')) ||
        text.contains(QLatin1Char('\n')) || text.contains(QLatin1Char('\r'));
    if (!needs_quotes) {
        return text;
    }
    QString quoted = text;
    quoted.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    return QStringLiteral("\"") + quoted + QStringLiteral("\"");
}

QString render(
    const tinydbms::core::QueryResult& query,
    QChar separator,
    const QString& line_ending) {
    if (query.columns.empty()) {
        return QString();
    }

    QStringList lines;
    lines.reserve(static_cast<qsizetype>(query.rows.size()) + 1);
    QStringList header;
    header.reserve(static_cast<qsizetype>(query.columns.size()));
    for (const tinydbms::core::ColumnHeader& column : query.columns) {
        header.append(escape_field(QString::fromUtf8(column.name), separator));
    }
    lines.append(header.join(separator));

    for (const tinydbms::core::Row& row : query.rows) {
        QStringList cells;
        cells.reserve(static_cast<qsizetype>(query.columns.size()));
        for (std::size_t index = 0; index < query.columns.size(); ++index) {
            // 与表格同一路径：缺失单元格按空字段导出，不中断整份导出。
            const QString text = index < row.size() ? value_text(row[index]) : QString();
            cells.append(escape_field(text, separator));
        }
        lines.append(cells.join(separator));
    }

    return lines.join(line_ending) + line_ending;
}

}  // namespace

QString csv_text(const tinydbms::core::QueryResult& query) {
    return render(query, QLatin1Char(','), QStringLiteral("\r\n"));
}

QString tsv_text(const tinydbms::core::QueryResult& query) {
    return render(query, QLatin1Char('\t'), QStringLiteral("\n"));
}

QByteArray csv_bytes(const tinydbms::core::QueryResult& query) {
    const QString text = csv_text(query);
    if (text.isEmpty()) {
        return QByteArray{};
    }
    return QByteArray{"\xEF\xBB\xBF"} + text.toUtf8();
}

}  // namespace tinydbms::gui
