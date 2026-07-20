#include "DropTargets.h"
#include <QMimeData>
#include <QRegularExpression>
#include <QSet>

bool isDownloadableScheme(const QUrl& u) {
    if (!u.isValid() || u.host().isEmpty()) return false;
    const QString s = u.scheme().toLower();
    return s == "http" || s == "https" || s == "ftp";
}

static void appendIfNew(QList<QUrl>& out, QSet<QString>& seen, const QUrl& u) {
    if (!isDownloadableScheme(u)) return;
    const QString key = u.toString();
    if (seen.contains(key)) return;
    seen.insert(key);
    out.append(u);
}

QList<QUrl> extractUrls(const QMimeData* mime) {
    QList<QUrl>   out;
    QSet<QString> seen;
    if (!mime) return out;

    if (mime->hasUrls())
        for (const QUrl& u : mime->urls()) appendIfNew(out, seen, u);

    if (mime->hasText()) {
        // Varre o texto por tokens que pareçam URL. Arrastar uma SELEÇÃO do
        // navegador cai aqui (não em uri-list), e o texto pode ter lixo em volta.
        static const QRegularExpression re(R"((?:https?|ftp)://[^\s<>"']+)",
                                           QRegularExpression::CaseInsensitiveOption);
        auto it = re.globalMatch(mime->text());
        while (it.hasNext()) appendIfNew(out, seen, QUrl(it.next().captured(0)));
    }
    return out;
}
