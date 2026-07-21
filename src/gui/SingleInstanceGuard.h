#pragma once
#include <QObject>
#include <QString>
class QLocalServer;

inline constexpr char kShowMessage[] = "show";   // foreground second launch -> reveal window
inline constexpr char kPingMessage[] = "ping";   // background second launch -> just exit

class SingleInstanceGuard : public QObject {
    Q_OBJECT
public:
    explicit SingleInstanceGuard(QString serverName, QObject* parent = nullptr);
    // No primary yet -> start listening and return true. Otherwise send
    // secondaryMessage to the existing primary and return false.
    bool tryBecomePrimary(const QByteArray& secondaryMessage);
signals:
    void showRequested();
private:
    void onNewConnection();
    QString       m_name;
    QLocalServer* m_server = nullptr;
};
