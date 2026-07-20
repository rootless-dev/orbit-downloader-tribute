#pragma once
#include "DownloadTypes.h"
#include <QObject>
#include <QString>
#include <QUrl>
class QFile;
class RateLimiter;

// Credenciais de protocolo. Vazio = anônimo. Trafega por parâmetro (nunca no
// Transport, que é compartilhado por todas as tasks) porque credenciais vindas
// do diálogo vivem só em memória, na DownloadTask. HttpTransport ignora por ora.
struct Credentials {
    QString user;
    QString pass;
    bool isEmpty() const { return user.isEmpty() && pass.isEmpty(); }
};

// `failed` só é emitido quando o worker DESISTE; o retry recuperável é interno
// ao worker. Logo só existem dois desfechos que atravessam a interface: erro
// fatal, ou "pare e peça credenciais". AuthRequired existe porque no resume o
// probe não roda e o 530 chega pelo worker (spec §3.5/§3.6).
enum class FailureKind { Fatal, AuthRequired };

class Probe : public QObject {
    Q_OBJECT
public:
    explicit Probe(QObject* parent = nullptr) : QObject(parent) {}
    virtual void start(const QUrl& url, const Credentials& creds,
                       const HeaderList& extraHeaders) = 0;
signals:
    void finished(const ProbeResult& result);
};

class SegmentSource : public QObject {
    Q_OBJECT
public:
    explicit SegmentSource(QObject* parent = nullptr) : QObject(parent) {}
    virtual void start(const Segment& seg, const QUrl& url,
                       const QString& validator, const Credentials& creds,
                       const HeaderList& extraHeaders) = 0;
    virtual void stop() = 0;
    virtual Segment segment() const = 0;
signals:
    void progressed(int index, qint64 currentOffset);
    void completed(int index);
    void failed(int index, const QString& error, FailureKind kind);
    void restartRequired(int index);
};

// Não é QObject: sem sinais, sem filhos. Os objetos que cria recebem o parent
// passado (a DownloadTask), preservando a árvore de ownership atual.
class Transport {
public:
    virtual ~Transport() = default;
    virtual Probe*         createProbe(const EngineConfig& cfg, QObject* parent) = 0;
    virtual SegmentSource* createWorker(QFile* file, const EngineConfig& cfg,
                                        RateLimiter* limiter, QObject* parent) = 0;
};

// Necessário p/ QSignalSpy inspecionar o argumento de `failed` nos testes.
Q_DECLARE_METATYPE(FailureKind)
