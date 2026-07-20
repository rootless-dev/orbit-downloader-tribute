#include "UrlName.h"
#include <QFileInfo>

QString deriveFileName(const QUrl& url) {
    const QString path = url.path(QUrl::FullyDecoded);  // decodes %20 etc.
    const QString name = QFileInfo(path).fileName();
    return name.isEmpty() ? QStringLiteral("download") : name;
}
