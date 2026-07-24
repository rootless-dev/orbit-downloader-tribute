#pragma once
#include "DownloadTypes.h"
#include "AbstractTask.h"   // AbstractTask::Kind
#include <QStringList>

// Task 14: torrent-only context-menu entries — the two piece-strategy choices
// and Force re-check. Returns an empty list for non-torrent kinds so the
// existing HTTP/FTP menu stays unchanged. Pure -> testable headless.
inline QStringList ctxTorrentItems(AbstractTask::Kind kind) {
    if (kind != AbstractTask::Kind::Torrent) return {};
    return { QStringLiteral("Rarest-first"), QStringLiteral("Sequential"),
             QStringLiteral("Force re-check") };
}

// Regras de habilitação do menu de contexto (spec §3.4). Puras -> testáveis.
inline bool ctxCanStart(DownloadState s) {
    return s == DownloadState::Queued || s == DownloadState::Paused ||
           s == DownloadState::Cancelled || s == DownloadState::Error;
}
inline bool ctxCanStop(DownloadState s) {
    return s == DownloadState::Connecting || s == DownloadState::Downloading;
}
inline bool ctxCanCancel(DownloadState s) {
    return s != DownloadState::Completed && s != DownloadState::Cancelled;
}
inline bool ctxCanMove(DownloadState s) {
    return s != DownloadState::Downloading && s != DownloadState::Connecting;
}
inline bool ctxCanOpen(DownloadState s) {
    return s == DownloadState::Completed;
}
