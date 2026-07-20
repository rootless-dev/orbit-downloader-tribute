#pragma once
#include "DownloadTypes.h"

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
