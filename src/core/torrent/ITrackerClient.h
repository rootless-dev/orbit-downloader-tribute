#pragma once

#include "torrent/TrackerTypes.h"

#include <QByteArray>
#include <QObject>
#include <QUrl>
#include <QVector>

// Common contract for a single tracker (one instance = one tracker URL).
// AnnounceController owns a set of these and routes announces across them;
// TorrentTask never sees a concrete client, only the controller.
class ITrackerClient : public QObject {
    Q_OBJECT
public:
    explicit ITrackerClient(QObject* parent = nullptr) : QObject(parent) {}
    ~ITrackerClient() override = default;

    virtual QUrl trackerUrl() const = 0;

    // Fire one announce to this client's tracker. Emits peersReceived on a
    // well-formed response, announceFailed otherwise.
    virtual void announce(const QByteArray& infoHash, const QByteArray& peerId, quint16 port,
                          qint64 downloaded, qint64 left, TrackerEvent ev) = 0;

signals:
    // minIntervalSecs is BEP 3's tracker-mandated floor: the client MUST NOT
    // announce to this tracker faster than this, no matter how peer-starved
    // or impatient the adaptive cadence gets. 0 means the tracker didn't send
    // one (e.g. BEP 15 UDP has no min-interval field) and no floor applies.
    void peersReceived(QVector<PeerAddress> peers, int intervalSecs, int minIntervalSecs);
    void announceFailed(QString reason);
};
