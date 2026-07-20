#pragma once
#include <QObject>
#include <QString>
#include <QUuid>
#include <QHash>
class QFile;

enum class LogLevel { Debug, Info, Warn, Error };

// Logging do core: grava um app.log global + um arquivo por download em
// <dataDir>/logs/, e emite lineLogged por linha para a GUI mostrar ao vivo.
class Logger : public QObject {
    Q_OBJECT
public:
    explicit Logger(const QString& dataDir, QObject* parent = nullptr);
    ~Logger() override;

    void logApp(LogLevel level, const QString& msg);
    void logTask(const QUuid& id, const QString& destPath, LogLevel level, const QString& msg);

    QString logsDir() const { return m_logsDir; }
    QString taskLogPath(const QUuid& id, const QString& destPath) const;

signals:
    void lineLogged(const QUuid& id, const QString& formattedLine);

private:
    static QString levelStr(LogLevel l);
    QString formatLine(LogLevel level, const QString& msg) const;
    QFile*  fileFor(const QString& path);       // handle aberto (Append), em cache
    void    appendToFile(const QString& path, const QString& line);

    QString m_logsDir;
    qint64  m_appRotateBytes = 5LL * 1024 * 1024;
    QHash<QString, QFile*> m_open;              // path -> handle aberto (dono)
};
