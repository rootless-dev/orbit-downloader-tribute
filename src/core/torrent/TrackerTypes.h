#pragma once

#include <QMetaType>
#include <QString>
#include <QVector>

// One peer address as reported by a tracker (compact or dictionary form).
struct PeerAddress {
    QString host;
    quint16 port = 0;
};

// BEP 3 announce event, sent as the tracker's "event" query parameter.
// None omits the parameter entirely (a plain periodic re-announce).
enum class TrackerEvent { None, Started, Stopped, Completed };

Q_DECLARE_METATYPE(PeerAddress)
Q_DECLARE_METATYPE(QVector<PeerAddress>)
