#pragma once
#include "DownloadTypes.h"   // HeaderList (orbitcore)
#include <QByteArray>
#include <QString>
#include <QUrl>

// Origin permitido = a nossa extensão (ID fixado pela `key` do manifesto, Task 5/9).
// PÚBLICO (está no repo): não é segredo — o token é a credencial real (ver spec §6.1).
inline const QString kExtensionOrigin =
    QStringLiteral("chrome-extension://fpgmmgajdlkbhcecgjencmnnfeoigmkn");

struct AddRequest {
    QString    method;
    QString    path;
    QString    origin;                 // vazio se ausente
    QString    token;                  // X-Orbit-Token; vazio se ausente
    qint64     contentLength = -1;
    QByteArray body;
    bool       headersComplete = false;
    bool       bodyComplete    = false;
};
AddRequest parseAddRequest(const QByteArray& raw);

struct DownloadPayload {
    QUrl    url;
    QString filename, referrer, userAgent, cookie;
    bool    valid = false;             // url http/https presente
};
DownloadPayload parseBody(const QByteArray& json);
HeaderList headersFromPayload(const DownloadPayload& p);

enum class AuthResult { Ok, Unauthorized, Forbidden };
AuthResult authorize(const AddRequest& req, const QString& expectedToken,
                     const QString& allowedOrigin);

QString generateBridgeToken();         // 32 hex, QRandomGenerator::system()
