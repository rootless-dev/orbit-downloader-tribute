#include "ClipboardWatcher.h"
#include "DropTargets.h"        // isDownloadableScheme
#include "torrent/MagnetUri.h"
#include <QClipboard>
#include <QGuiApplication>

std::optional<QUrl> shouldOffer(const QString& text, const QUrl& lastOffered, bool selfCopied) {
    if (selfCopied) return std::nullopt;
    const QString t = text.trimmed();
    if (t.isEmpty()) return std::nullopt;
    const QUrl u(t);
    if (!isDownloadableScheme(u)) return std::nullopt;
    if (lastOffered.isValid() && u == lastOffered) return std::nullopt;
    return u;
}

std::optional<QString> shouldOfferMagnet(const QString& text, const QString& lastOffered,
                                         bool selfCopied) {
    if (selfCopied) return std::nullopt;
    const QString t = text.trimmed();
    if (t.isEmpty()) return std::nullopt;
    if (!MagnetUri::parse(t).isValid()) return std::nullopt;
    if (!lastOffered.isEmpty() && t == lastOffered) return std::nullopt;
    return t;
}

ClipboardWatcher::ClipboardWatcher(QObject* parent) : QObject(parent) {
    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged,
            this, &ClipboardWatcher::onClipboardChanged);
}

void ClipboardWatcher::markSelfCopy() { m_selfCopied = true; }

void ClipboardWatcher::onClipboardChanged() {
    const bool self = m_selfCopied;
    m_selfCopied = false;                       // consome a marca
    if (m_mode == ClipboardMode::Off) return;

    const QString text = QGuiApplication::clipboard()->text();
    if (const auto u = shouldOffer(text, m_lastOffered, self)) {
        m_lastOffered = *u;
        emit urlDetected(*u);
        return;
    }
    if (const auto m = shouldOfferMagnet(text, m_lastOfferedMagnet, self)) {
        m_lastOfferedMagnet = *m;
        emit magnetDetected(*m);
    }
}
