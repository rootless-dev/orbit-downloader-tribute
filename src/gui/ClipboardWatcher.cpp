#include "ClipboardWatcher.h"
#include "DropTargets.h"        // isDownloadableScheme
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

ClipboardWatcher::ClipboardWatcher(QObject* parent) : QObject(parent) {
    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged,
            this, &ClipboardWatcher::onClipboardChanged);
}

void ClipboardWatcher::markSelfCopy() { m_selfCopied = true; }

void ClipboardWatcher::onClipboardChanged() {
    const bool self = m_selfCopied;
    m_selfCopied = false;                       // consome a marca
    if (m_mode == ClipboardMode::Off) return;

    const auto u = shouldOffer(QGuiApplication::clipboard()->text(), m_lastOffered, self);
    if (!u) return;
    m_lastOffered = *u;
    emit urlDetected(*u);
}
