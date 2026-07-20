#pragma once
#include <QByteArray>
#include <QDateTime>
#include <QPair>
#include <QString>
#include <optional>

// Uma resposta FTP (RFC 959): "NNN <texto>\r\n", ou multi-linha
// "NNN-<texto>\r\n ... \r\nNNN <texto>\r\n".
struct FtpReply {
    int     code     = 0;
    QString text;               // texto da linha FINAL
    bool    complete = false;   // false = o buffer ainda não tem uma resposta inteira
};

// Extrai a primeira resposta completa de `buf`. Quando complete, *consumed é o
// número de bytes a descartar do buffer; quando não, *consumed = 0 e o chamador
// espera mais dados.
FtpReply parseReply(const QByteArray& buf, int* consumed);

// "227 Entering Passive Mode (h1,h2,h3,h4,p1,p2)" -> ("h1.h2.h3.h4", p1*256+p2)
std::optional<QPair<QString, quint16>> parsePasv(const QString& line);

// "213 YYYYMMDDHHMMSS" -> QDateTime (UTC)
std::optional<QDateTime> parseMdtm(const QString& line);
