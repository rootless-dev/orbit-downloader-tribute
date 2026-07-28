#pragma once

#include "AbstractTask.h"
#include "DownloadTypes.h"
#include "torrent/Bitfield.h"
#include "torrent/TrackerTypes.h" // PeerAddress
#include "torrent/PiecePicker.h"       // BlockRequest
#include "torrent/TorrentMetainfo.h"

#include <QByteArray>
#include <QHash>
#include <QMetaObject>
#include <QSet>
#include <QString>
#include <QUuid>
#include <QVector>

#include <memory>

class QNetworkAccessManager;
class QTimer;
class RateLimiter;
class Logger;
enum class LogLevel;
class PieceStore;
class PeerConnection;
class AnnounceController;
class DhtNode;

// Task 10: the BitTorrent leech engine.
//
// TorrentTask orchestrates the from-scratch client's building blocks into a
// working, verifying, resumable download: it drives a set of PeerConnections,
// asks the PiecePicker what to request (pipelined, depth 8 per peer),
// assembles each piece's blocks, hash-verifies it via PieceStore, writes it to
// disk, folds it into a Bitfield, and reports progress. Availability from
// peer bitfields/haves feeds the picker; completed pieces are persisted to a
// small on-disk resume file so a restart adopts prior progress instead of
// re-downloading. All I/O is event-loop / async Qt (no threads); nothing in
// the state machine depends on a global RNG or the wall clock (the peer_id is
// derived deterministically from rngSeed).
class TorrentTask : public AbstractTask {
    Q_OBJECT
public:
    TorrentTask(const TorrentMetainfo& m, const QString& destDir, const QSet<int>& selectedFiles,
                PieceStrategy strategy, quint16 listenPort, int maxPeers,
                ResumeVerifyMode verify, quint32 rngSeed,
                QNetworkAccessManager* nam, RateLimiter* limiter, Logger* logger,
                const QString& resumeDir, QObject* parent = nullptr);
    ~TorrentTask() override;

    // AbstractTask
    Kind          kind() const override { return Kind::Torrent; }
    QUuid         id() const override { return m_id; }
    DownloadState state() const override { return m_state; }
    QString       displayName() const override { return m_meta.name; }
    qint64        totalBytes() const override { return m_totalWantedBytes; }
    qint64        receivedBytes() const override { return m_verifiedBytes; }
    Priority      priority() const override { return m_priority; }
    void          setPriority(Priority p) override { m_priority = p; }
    void          start() override;
    void          pause() override;
    void          requeue() override;
    void          cancel() override;

    void                   setStrategy(PieceStrategy s);
    void                   forceRecheck();
    const TorrentMetainfo& metainfo() const { return m_meta; }
    // On-disk payload root for this torrent (PieceStore::destRootPath()):
    // <destDir>/<name> — the file itself for a single-file torrent, the
    // containing directory for a multi-file one. Used by DownloadManager to
    // delete the payload on remove(deleteFiles=true).
    QString                payloadRootPath() const;
    PieceState             pieceState(int i) const;
    // Config echoes (Task 15): let tests/callers observe the values this task
    // was actually constructed with, e.g. to confirm DownloadManager's
    // Preferences-sourced defaults reached the engine.
    int                    maxPeers() const { return m_maxPeers; }
    quint16                listenPort() const { return m_listenPort; }
    ResumeVerifyMode       verifyMode() const { return m_verifyMode; }
    void                   restoreBitfield(); // read <resumeDir>/<hexInfoHash>.bitfield if present
    void                   addPeerForTest(const PeerAddress& p); // TEST SEAM: inject a peer, no tracker

    // Task 14: DHT as a peer source. `dht` is NOT owned/parented here -- it is
    // a single instance DownloadManager shares across every TorrentTask (or,
    // in tests, one the caller keeps alive on the stack), and it outlives
    // this task. Passing nullptr detaches from a previously-set DhtNode
    // (unsubscribes, if currently subscribed). If the task is already
    // Connecting/Downloading when this is called, subscribes to
    // DhtNode::peersFound immediately and kicks off a lookup() for this
    // torrent's info_hash; otherwise beginLeeching() does both when the task
    // starts running.
    void                   setDht(DhtNode* dht);

    // Diagnostics (peer/tracker observability): exposed for the GUI
    // Properties panel and the per-download Log tab's heartbeat line.
    int     connectedPeerCount() const { return m_peers.size(); }
    int     unchokedPeerCount() const;
    QString trackerStatus() const { return m_trackerStatus; }

    // TEST SEAM: thin wrapper so tst_torrent can exercise the pure re-announce
    // cadence function without duplicating its logic or depending on any
    // TorrentTask instance/state. minIntervalSecs is the tracker's BEP 3
    // "min interval" (0 if never sent by any tracker) — the cadence must never
    // announce faster than this, however peer-starved we are.
    static int nextAnnounceDelaySecsForTest(int connectedPeers, bool hasWantedProgress,
                                            int trackerIntervalSecs, int minIntervalSecs);

signals:
    void pieceStateChanged(int piece, PieceState st);

private:
    void setState(DownloadState s);
    void runChecking();                              // verifyOnDisk each wanted piece -> bitfield
    void adoptHaveBitfield(const Bitfield& bf, bool emitStates);
    void beginLeeching();                            // Connecting -> announce -> open peers
    void openPeers();
    void wirePeer(PeerConnection* pc);
    void onHandshake(PeerConnection* pc);
    void onUnchoked(PeerConnection* pc);
    void onBlock(PeerConnection* pc, int piece, qint64 begin, const QByteArray& data);
    void onPeerDisconnected(PeerConnection* pc);
    void dropPeer(PeerConnection* pc); // release in-flight blocks, unwire, delete
    void requestMore(PeerConnection* pc);
    void pumpPeers();                  // top up EVERY unchoked peer's pipeline
    void completePiece(int piece, const QByteArray& whole);
    bool failPiece(PeerConnection* pc, int piece); // returns true if the peer was dropped
    void setPieceState(int piece, PieceState st);
    bool haveAllWanted() const;
    void maybeFinish();
    void scheduleProgress();
    void emitProgressNow();
    void scheduleResumeSave();
    void saveResumeNow();
    QString resumeFilePath() const;
    int     expectedBlockCount(int piece) const;
    qint64  pieceWantedBytes(int piece) const;
    void    teardownPeers();
    void    logLine(LogLevel level, const QString& msg);
    void    rescheduleAnnounceTimer(); // adaptive cadence (part C): starved -> re-announce often
    void    subscribeDht();            // (re)connects to m_dht->peersFound, filtered to our info_hash
    void    unsubscribeDht();          // disconnects m_dhtPeersConn if live; safe to call when not connected

    // Config / collaborators (not owned unless noted).
    TorrentMetainfo         m_meta;
    QString                 m_destDir;
    QSet<int>               m_selectedFiles;
    PieceStrategy           m_strategy;
    quint16                 m_listenPort;
    int                     m_maxPeers;
    ResumeVerifyMode        m_verifyMode;
    quint32                 m_rngSeed;
    QNetworkAccessManager*  m_nam;
    RateLimiter*            m_limiter;
    Logger*                 m_logger;
    QString                 m_resumeDir;

    QUuid                   m_id;
    QByteArray              m_peerId;
    DownloadState           m_state = DownloadState::Queued;
    Priority                m_priority = Priority::Normal;

    int                          m_pieceCount = 0;
    std::unique_ptr<PieceStore>  m_store;   // owned
    std::unique_ptr<PiecePicker> m_picker;  // owned
    AnnounceController*          m_announce = nullptr; // owned (QObject child)
    DhtNode*                     m_dht = nullptr;       // NOT owned; shared, outlives this task
    QMetaObject::Connection      m_dhtPeersConn;        // live only while subscribed to m_dht->peersFound
    Bitfield                     m_have;
    QSet<int>                    m_wanted;
    int                          m_wantedHaveCount = 0;
    qint64                       m_totalWantedBytes = 0;
    qint64                       m_verifiedBytes = 0;
    bool                         m_forceRecheck = false;
    bool                         m_bitfieldRestored = false;

    QVector<PieceState> m_pieceStates;

    // In-progress piece assembly.
    QHash<int, QByteArray>    m_pieceBuf;    // piece -> full-size buffer
    QHash<int, QSet<qint64>>  m_pieceBlocks; // piece -> received block begins

    // Peers.
    QVector<PeerConnection*>                        m_peers;       // owned (QObject children)
    QVector<PeerAddress>                            m_pendingPeers;
    QSet<QString>                                   m_knownPeers;  // dedupe host:port
    QHash<PeerConnection*, QVector<BlockRequest>>   m_inflightByPeer;
    QHash<PeerConnection*, int>                     m_badPieces;
    QSet<PeerConnection*>                           m_firstBlockLogged; // one "receiving data" log/peer

    bool    m_pumping = false; // re-entrancy guard for pumpPeers()

    QTimer* m_progressTimer = nullptr;
    bool    m_progressPending = false;
    QTimer* m_resumeTimer = nullptr;
    bool    m_resumePending = false;
    QTimer* m_announceTimer = nullptr;
    int     m_announceIntervalSecs = 0;
    int     m_minAnnounceIntervalSecs = 0; // tracker's BEP 3 "min interval" floor (0 if none sent)
    QString m_trackerStatus = QStringLiteral("not announced");
    QTimer* m_heartbeatTimer = nullptr; // ~5s diagnostics line while Connecting/Downloading
};
