#include <QtTest>
#include "torrent/MagnetUri.h"

class TstMagnetUri : public QObject {
    Q_OBJECT
private slots:
    void parsesHexInfoHashNameAndTrackers() {
        const QString uri = "magnet:?xt=urn:btih:143b885127dfa398b9c58f4abc7f3145b91f5f4f"
                            "&dn=ubuntu-24.04.4-desktop-arm64.iso"
                            "&tr=https%3A%2F%2Ftorrent.ubuntu.com%2Fannounce"
                            "&tr=https%3A%2F%2Fipv6.torrent.ubuntu.com%2Fannounce";
        const MagnetInfo m = MagnetUri::parse(uri);
        QVERIFY(m.isValid());
        QCOMPARE(m.infoHash.toHex(), QByteArray("143b885127dfa398b9c58f4abc7f3145b91f5f4f"));
        QCOMPARE(m.displayName, QString("ubuntu-24.04.4-desktop-arm64.iso"));
        QCOMPARE(m.trackers.size(), 2);
        QCOMPARE(m.trackers.at(0), QString("https://torrent.ubuntu.com/announce"));
    }
    void parsesBase32InfoHash() {
        // base32 of the same 20 bytes (32 chars, RFC 4648, uppercase)
        const QString uri = "magnet:?xt=urn:btih:CQ5YQUJH36RZROOFR5FLY7ZRIW4R6X2P";
        const MagnetInfo m = MagnetUri::parse(uri);
        QVERIFY(m.isValid());
        QCOMPARE(m.infoHash.toHex(), QByteArray("143b885127dfa398b9c58f4abc7f3145b91f5f4f"));
    }
    void rejectsNonMagnetOrMissingXt() {
        QVERIFY(!MagnetUri::parse("https://example.com").isValid());
        QVERIFY(!MagnetUri::parse("magnet:?dn=foo").isValid());
        QVERIFY(!MagnetUri::parse("magnet:?xt=urn:btih:zzzz").isValid()); // bad hash
    }
};
QTEST_APPLESS_MAIN(TstMagnetUri)
#include "tst_magneturi.moc"
