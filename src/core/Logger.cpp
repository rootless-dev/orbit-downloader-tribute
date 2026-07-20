#include "Logger.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QRegularExpression>

Logger::Logger(const QString& dataDir, QObject* parent)
    : QObject(parent), m_logsDir(dataDir + "/logs") {
    QDir().mkpath(m_logsDir);
}

Logger::~Logger() {
    qDeleteAll(m_open);   // ~QFile fecha cada handle
}

QString Logger::levelStr(LogLevel l) {
    switch (l) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
    }
    return "INFO";
}

QString Logger::formatLine(LogLevel level, const QString& msg) const {
    const QString ts = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz");
    return QString("%1 [%2] %3").arg(ts, levelStr(level), msg);
}

// Mantém um handle aberto por arquivo (evita open/close por linha). O flush
// após cada escrita garante que a GUI/testes que releem o arquivo enxerguem a
// linha imediatamente.
QFile* Logger::fileFor(const QString& path) {
    auto it = m_open.find(path);
    if (it != m_open.end()) return it.value();
    auto* f = new QFile(path);
    if (!f->open(QIODevice::Append | QIODevice::Text)) { delete f; return nullptr; }
    m_open.insert(path, f);
    return f;
}

void Logger::appendToFile(const QString& path, const QString& line) {
    QFile* f = fileFor(path);
    if (!f) return;                 // best-effort
    f->write(line.toUtf8());
    f->write("\n");
    f->flush();
}

// Sanitiza o basename do destino e anexa os 8 primeiros hex do id -> único e legível.
QString Logger::taskLogPath(const QUuid& id, const QString& destPath) const {
    QString base = QFileInfo(destPath).completeBaseName();
    base.replace(QRegularExpression("[^A-Za-z0-9._-]"), "_");
    if (base.isEmpty()) base = "download";
    const QString shortId = id.toString(QUuid::WithoutBraces).left(8);
    return m_logsDir + "/" + base + "-" + shortId + ".log";
}

void Logger::logApp(LogLevel level, const QString& msg) {
    const QString appLog = m_logsDir + "/app.log";
    if (QFileInfo(appLog).size() >= m_appRotateBytes) {
        // Fecha o handle em cache ANTES de renomear: com o handle aberto, as
        // escritas seguintes iriam para o arquivo já rotacionado (app.log.1).
        if (QFile* old = m_open.take(appLog)) delete old;
        QFile::remove(appLog + ".1");
        QFile::rename(appLog, appLog + ".1");
    }
    const QString line = formatLine(level, msg);
    appendToFile(appLog, line);
    emit lineLogged(QUuid(), line);
}

void Logger::logTask(const QUuid& id, const QString& destPath, LogLevel level, const QString& msg) {
    const QString line = formatLine(level, msg);
    appendToFile(taskLogPath(id, destPath), line);
    emit lineLogged(id, line);
}
