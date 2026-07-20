#include "FtpReply.h"
#include <QRegularExpression>
#include <QStringList>
#include <QTimeZone>

static bool isCodeLine(const QByteArray& line, int code, char sep) {
    // "NNN<sep>..." com NNN == code
    if (line.size() < 4) return false;
    bool ok = false;
    const int c = line.left(3).toInt(&ok);
    return ok && c == code && line[3] == sep;
}

FtpReply parseReply(const QByteArray& buf, int* consumed) {
    if (consumed) *consumed = 0;
    FtpReply r;

    const int firstEol = buf.indexOf("\r\n");
    if (firstEol < 0) return r;                 // nem uma linha inteira ainda
    const QByteArray firstLine = buf.left(firstEol);
    if (firstLine.size() < 4) return r;

    bool ok = false;
    const int code = firstLine.left(3).toInt(&ok);
    if (!ok) return r;

    // Resposta de uma linha: "NNN texto"
    if (firstLine[3] == ' ') {
        r.code     = code;
        r.text     = QString::fromUtf8(firstLine.mid(4)).trimmed();
        r.complete = true;
        if (consumed) *consumed = firstEol + 2;
        return r;
    }

    // Multi-linha: "NNN-..." termina numa linha "NNN <texto>"
    if (firstLine[3] != '-') return r;
    int pos = firstEol + 2;
    while (true) {
        const int eol = buf.indexOf("\r\n", pos);
        if (eol < 0) return r;                  // terminador ainda não chegou
        const QByteArray line = buf.mid(pos, eol - pos);
        if (isCodeLine(line, code, ' ')) {
            r.code     = code;
            r.text     = QString::fromUtf8(line.mid(4)).trimmed();
            r.complete = true;
            if (consumed) *consumed = eol + 2;
            return r;
        }
        pos = eol + 2;
    }
}

std::optional<QPair<QString, quint16>> parsePasv(const QString& line) {
    static const QRegularExpression re(
        R"(\((\d{1,3}),(\d{1,3}),(\d{1,3}),(\d{1,3}),(\d{1,3}),(\d{1,3})\))");
    const auto m = re.match(line);
    if (!m.hasMatch()) return std::nullopt;

    int v[6];
    for (int i = 0; i < 6; ++i) {
        bool ok = false;
        v[i] = m.captured(i + 1).toInt(&ok);
        if (!ok || v[i] < 0 || v[i] > 255) return std::nullopt;
    }
    const QString host = QString("%1.%2.%3.%4").arg(v[0]).arg(v[1]).arg(v[2]).arg(v[3]);
    const quint16 port = quint16(v[4] * 256 + v[5]);
    return QPair<QString, quint16>(host, port);
}

std::optional<QDateTime> parseMdtm(const QString& line) {
    static const QRegularExpression re(R"(^213\s+(\d{14}))");
    const auto m = re.match(line.trimmed());
    if (!m.hasMatch()) return std::nullopt;
    QDateTime dt = QDateTime::fromString(m.captured(1), "yyyyMMddHHmmss");
    if (!dt.isValid()) return std::nullopt;
    dt.setTimeZone(QTimeZone::UTC);            // MDTM é sempre UTC (RFC 3659)
    return dt;
}
