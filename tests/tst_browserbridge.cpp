#include <QtTest/QtTest>
#include "BrowserBridgeProtocol.h"
#include "BrowserBridge.h"
#include <QTcpSocket>
#include <QSignalSpy>
#include <QHostAddress>

class TestBrowserBridge : public QObject {
    Q_OBJECT
private slots:
    void parsesWellFormedPost() {
        const QByteArray raw =
            "POST /add HTTP/1.1\r\nHost: 127.0.0.1:8697\r\n"
            "Origin: chrome-extension://abc\r\nX-Orbit-Token: tok\r\n"
            "Content-Type: application/json\r\nContent-Length: 2\r\n\r\n{}";
        const AddRequest r = parseAddRequest(raw);
        QVERIFY(r.headersComplete && r.bodyComplete);
        QCOMPARE(r.method, QString("POST"));
        QCOMPARE(r.path, QString("/add"));
        QCOMPARE(r.origin, QString("chrome-extension://abc"));
        QCOMPARE(r.token, QString("tok"));
        QCOMPARE(r.body, QByteArray("{}"));
    }
    void reportsIncompleteBody() {
        const QByteArray raw =
            "POST /add HTTP/1.1\r\nContent-Length: 10\r\n\r\n{}";   // só 2 dos 10
        const AddRequest r = parseAddRequest(raw);
        QVERIFY(r.headersComplete);
        QVERIFY(!r.bodyComplete);
    }
    void parsesBodyUrlAndFields() {
        const DownloadPayload p = parseBody(
            R"({"url":"https://h/f.zip","filename":"f.zip","referrer":"https://h/p",)"
            R"("userAgent":"UA","cookie":"a=1"})");
        QVERIFY(p.valid);
        QCOMPARE(p.url, QUrl("https://h/f.zip"));
        QCOMPARE(p.filename, QString("f.zip"));
        const HeaderList h = headersFromPayload(p);
        QVERIFY(h.contains({QByteArray("Cookie"), QByteArray("a=1")}));
        QVERIFY(h.contains({QByteArray("Referer"), QByteArray("https://h/p")}));
        QVERIFY(h.contains({QByteArray("User-Agent"), QByteArray("UA")}));
    }
    void rejectsNonHttpUrl() {
        const QByteArray fileUrlJson = R"({"url":"file:///etc/passwd"})";
        const QByteArray noUrlJson = R"({"foo":1})";
        QVERIFY(!parseBody(fileUrlJson).valid);
        QVERIFY(!parseBody(noUrlJson).valid);          // sem url
    }
    void authorizeAcceptsMatchingOriginAndToken() {
        AddRequest r; r.origin = "chrome-extension://x"; r.token = "s3cret";
        QCOMPARE(authorize(r, "s3cret", "chrome-extension://x"), AuthResult::Ok);
    }
    void authorizeAcceptsAbsentOrigin() {                    // extensão host-permission
        AddRequest r; r.origin = ""; r.token = "s3cret";
        QCOMPARE(authorize(r, "s3cret", "chrome-extension://x"), AuthResult::Ok);
    }
    void authorizeRejectsWrongOrigin() {
        AddRequest r; r.origin = "https://evil.test"; r.token = "s3cret";
        QCOMPARE(authorize(r, "s3cret", "chrome-extension://x"), AuthResult::Forbidden);
    }
    void authorizeRejectsBadToken() {
        AddRequest r; r.origin = "chrome-extension://x"; r.token = "nope";
        QCOMPARE(authorize(r, "s3cret", "chrome-extension://x"), AuthResult::Unauthorized);
        AddRequest empty; empty.token = "anything";
        QCOMPARE(authorize(empty, "", "chrome-extension://x"), AuthResult::Unauthorized);
    }
    void tokenIs32Hex() {
        const QString t = generateBridgeToken();
        QCOMPARE(t.size(), 32);
        QVERIFY(QRegularExpression("^[0-9a-f]{32}$").match(t).hasMatch());
    }
    void loopbackAcceptsValidPost() {
        BrowserBridge b;
        QVERIFY(b.start(0, "tok", kExtensionOrigin));   // porta 0 = efêmera
        QSignalSpy spy(&b, &BrowserBridge::downloadRequested);
        const QByteArray body = R"({"url":"https://h/f.zip","cookie":"a=1"})";
        QTcpSocket s; s.connectToHost(QHostAddress::LocalHost, b.port());
        QVERIFY(s.waitForConnected(2000));
        const QByteArray req = "POST /add HTTP/1.1\r\nHost: x\r\n"
            "Origin: " + kExtensionOrigin.toUtf8() + "\r\nX-Orbit-Token: tok\r\n"
            "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body;
        s.write(req); QVERIFY(s.waitForBytesWritten(2000));
        QVERIFY(spy.wait(2000));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toUrl(), QUrl("https://h/f.zip"));
        QVERIFY(s.waitForReadyRead(2000));
        QVERIFY(s.readAll().startsWith("HTTP/1.1 200"));
    }
    void loopbackRejectsBadToken() {
        BrowserBridge b; QVERIFY(b.start(0, "tok", kExtensionOrigin));
        QSignalSpy spy(&b, &BrowserBridge::downloadRequested);
        const QByteArray body = R"({"url":"https://h/f.zip"})";
        QTcpSocket s; s.connectToHost(QHostAddress::LocalHost, b.port());
        QVERIFY(s.waitForConnected(2000));
        s.write("POST /add HTTP/1.1\r\nX-Orbit-Token: WRONG\r\nContent-Length: "
                + QByteArray::number(body.size()) + "\r\n\r\n" + body);
        QTRY_VERIFY(s.bytesAvailable() > 0);
        QVERIFY(s.readAll().startsWith("HTTP/1.1 401"));
        QCOMPARE(spy.count(), 0);
    }
    void loopbackPreflightHasCorsAndPna() {
        BrowserBridge b; QVERIFY(b.start(0, "tok", kExtensionOrigin));
        QTcpSocket s; s.connectToHost(QHostAddress::LocalHost, b.port());
        QVERIFY(s.waitForConnected(2000));
        s.write("OPTIONS /add HTTP/1.1\r\nOrigin: " + kExtensionOrigin.toUtf8()
                + "\r\nAccess-Control-Request-Private-Network: true\r\n\r\n");
        QTRY_VERIFY(s.bytesAvailable() > 0);
        const QByteArray resp = s.readAll();
        QVERIFY(resp.startsWith("HTTP/1.1 204"));
        QVERIFY(resp.contains("Access-Control-Allow-Private-Network: true"));
        QVERIFY(resp.contains("Access-Control-Allow-Origin: " + kExtensionOrigin.toUtf8()));
    }
};
QTEST_MAIN(TestBrowserBridge)
#include "tst_browserbridge.moc"
