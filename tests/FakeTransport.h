#pragma once
#include "Transport.h"
#include <QByteArray>
#include <QFile>
#include <QTimer>

// Probe sintético: devolve um ProbeResult programado, assincronamente.
class FakeProbe : public Probe {
    Q_OBJECT
public:
    FakeProbe(ProbeResult r, Credentials* sink, int* count, QObject* parent = nullptr)
        : Probe(parent), m_r(r), m_sink(sink), m_count(count) {}
    void start(const QUrl& url, const Credentials& creds,
              const HeaderList& extraHeaders) override {
        Q_UNUSED(url);
        Q_UNUSED(extraHeaders);
        if (m_sink)  *m_sink = creds;
        if (m_count) ++(*m_count);
        QTimer::singleShot(0, this, [this] { emit finished(m_r); });
    }
private:
    ProbeResult  m_r;
    Credentials* m_sink;
    int*         m_count;
};

// Worker sintético: escreve a fatia [start, end] do corpo programado e completa.
class FakeWorker : public SegmentSource {
    Q_OBJECT
public:
    FakeWorker(QByteArray body, QFile* file, QObject* parent = nullptr)
        : SegmentSource(parent), m_body(body), m_file(file) {}
    void start(const Segment& seg, const QUrl& url,
               const QString& validator, const Credentials& creds,
               const HeaderList& extraHeaders) override {
        Q_UNUSED(url); Q_UNUSED(validator); Q_UNUSED(creds); Q_UNUSED(extraHeaders);
        m_seg = seg;
        QTimer::singleShot(0, this, [this] {
            if (m_stopped) return;
            const qint64 end = (m_seg.end >= 0) ? m_seg.end : m_body.size() - 1;
            const qint64 n   = end - m_seg.current + 1;
            if (n > 0) {
                m_file->seek(m_seg.current);
                m_file->write(m_body.constData() + m_seg.current, n);
                m_seg.current += n;
                emit progressed(m_seg.index, m_seg.current);
            }
            if (m_seg.end < 0) m_seg.end = m_seg.current - 1;
            emit completed(m_seg.index);
        });
    }
    void stop() override { m_stopped = true; }
    Segment segment() const override { return m_seg; }
private:
    QByteArray m_body;
    QFile*     m_file;
    Segment    m_seg;
    bool       m_stopped = false;
};

// Worker que pede restart enquanto houver validador, e baixa normalmente
// quando não houver. (Não precisa de flag "já reiniciei": onRestartRequired
// limpa o validador antes de respawnar, então a segunda rodada chega aqui com
// validator vazio — é assim que o restart é finito, spec §3.5.)
// Depois do emit, escreve em m_touched: se o objeto tiver sido destruído
// durante o emit, isto é use-after-free (ASAN pega; sem ASAN, corrompe).
class RestartingWorker : public SegmentSource {
    Q_OBJECT
public:
    RestartingWorker(QByteArray body, QFile* file, QObject* parent = nullptr)
        : SegmentSource(parent), m_body(body), m_file(file) {}
    void start(const Segment& seg, const QUrl& url,
               const QString& validator, const Credentials& creds,
               const HeaderList& extraHeaders) override {
        Q_UNUSED(url); Q_UNUSED(creds); Q_UNUSED(extraHeaders);
        m_seg = seg;
        m_askRestart = !validator.isEmpty();
        QTimer::singleShot(0, this, [this] {
            if (m_stopped) return;
            if (m_askRestart) {
                emit restartRequired(m_seg.index);
                m_touched = 42;          // <-- use-after-free se fomos destruídos no emit
                return;
            }
            const qint64 end = (m_seg.end >= 0) ? m_seg.end : m_body.size() - 1;
            const qint64 n   = end - m_seg.current + 1;
            if (n > 0) {
                m_file->seek(m_seg.current);
                m_file->write(m_body.constData() + m_seg.current, n);
                m_seg.current += n;
                emit progressed(m_seg.index, m_seg.current);
            }
            if (m_seg.end < 0) m_seg.end = m_seg.current - 1;
            emit completed(m_seg.index);
        });
    }
    void stop() override { m_stopped = true; }
    Segment segment() const override { return m_seg; }
private:
    QByteArray m_body;
    QFile*     m_file;
    Segment    m_seg;
    bool       m_askRestart = false;
    bool       m_stopped    = false;
    int        m_touched    = 0;
};

class FakeTransport : public Transport {
public:
    Probe*         createProbe(const EngineConfig& cfg, QObject* parent) override;
    SegmentSource* createWorker(QFile* file, const EngineConfig& cfg,
                                RateLimiter* limiter, QObject* parent) override;
    void setProbeResult(const ProbeResult& r) { m_probeResult = r; }
    void setBody(const QByteArray& b)         { m_body = b; }
    Credentials lastCredentials() const       { return m_lastCreds; }
    int         probeCount() const            { return m_probeCount; }
    void setRestartOnce(bool on)              { m_restartOnce = on; }
private:
    ProbeResult m_probeResult;
    QByteArray  m_body;
    Credentials m_lastCreds;
    int         m_probeCount = 0;
    bool        m_restartOnce = false;
};
