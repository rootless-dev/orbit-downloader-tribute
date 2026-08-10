#pragma once
#include <QByteArray>
#include <QString>
#include <QStringList>

struct MagnetInfo {
    QByteArray infoHash;
    QString    displayName;
    QStringList trackers;
    bool isValid() const { return infoHash.size() == 20; }
};

namespace MagnetUri { MagnetInfo parse(const QString& uri); }
