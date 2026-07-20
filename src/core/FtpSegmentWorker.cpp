#include "FtpSegmentWorker.h"
#include "FtpReply.h"
#include "RateLimiter.h"
#include <QFile>
#include <QTcpSocket>
#include <QTimer>
#include <QDateTime>

FtpSegmentWorker::FtpSegmentWorker(QFile* file, const EngineConfig& cfg, RateLimiter* limiter,
                                   QObject* parent)
    : SegmentSource(parent), m_file(file), m_cfg(cfg), m_limiter(limiter) {}

void FtpSegmentWorker::start(const Segment& seg, const QUrl& url,
                             const QString& validator, const Credentials& creds,
                             const HeaderList& extraHeaders) {
    Q_UNUSED(extraHeaders);
    m_seg       = seg;
    m_url       = url;
    m_validator = validator;
    m_creds     = creds;
    m_stopped   = false;
    m_finished  = false;
    m_attempt   = 0;
    openAttempt();
}

void FtpSegmentWorker::openAttempt() {
    if (m_stopped) return;
    if (m_seg.isComplete()) { emit completed(m_seg.index); return; }

    teardown();                       // limpa qualquer tentativa anterior
    m_step = Step::Pasv;
    m_ch   = new FtpControlChannel(this);
    connect(m_ch, &FtpControlChannel::loggedIn,      this, &FtpSegmentWorker::onLoggedIn);
    connect(m_ch, &FtpControlChannel::replyReceived, this, &FtpSegmentWorker::onReply);
    connect(m_ch, &FtpControlChannel::failed,        this, &FtpSegmentWorker::onControlFailed);
    m_ch->connectAndLogin(m_url, m_creds, m_cfg.connectTimeoutMs);
    armIdleTimer(m_cfg.connectTimeoutMs);
}

void FtpSegmentWorker::onLoggedIn() {
    if (m_stopped) return;
    // FTP não tem If-Range. Quando há validador (mesma condição em que o HTTP
    // mandaria If-Range), verificamos explicitamente que o arquivo não mudou —
    // um round-trip a mais, que é o preço do protocolo (spec §3.5).
    if (!m_validator.isEmpty()) {
        m_step = Step::Mdtm;
        m_ch->sendCommand("MDTM " + m_url.path());
        return;
    }
    m_step = Step::Pasv;
    m_ch->sendCommand("PASV");
}

void FtpSegmentWorker::onReply(int code, const QString& text) {
    if (m_stopped || m_finished) return;

    switch (m_step) {
        case Step::Mdtm: {
            // Servidor sem MDTM (502): não dá p/ validar. Seguimos — é a mesma
            // exposição do HTTP sem ETag/Last-Modified (spec §3.5).
            if (code != 213) {
                m_step = Step::Pasv;
                m_ch->sendCommand("PASV");
                return;
            }
            if (text.trimmed() != m_validator) {
                // O arquivo mudou: o parcial no disco não presta. restartRequired
                // (não failed): dá p/ recomeçar do zero. A DownloadTask chama
                // stop()+deleteLater() neste worker de dentro do emit (Task 3):
                // retorne imediatamente e NÃO toque em membros depois daqui.
                emit restartRequired(m_seg.index);
                return;
            }
            m_step = Step::Pasv;
            m_ch->sendCommand("PASV");
            return;
        }

        case Step::Pasv: {
            if (code != 227) { scheduleRetry(QString("PASV: %1 %2").arg(code).arg(text)); return; }
            const auto hp = parsePasv(QString("227 %1").arg(text));
            if (!hp) { scheduleRetry("PASV: resposta malformada"); return; }

            m_data = new QTcpSocket(this);
            if (m_limiter && m_limiter->rate() > 0)
                m_data->setReadBufferSize(qMax<qint64>(m_limiter->rate(), 64 * 1024));  // ~1s buffer, backpressure via TCP
            connect(m_data, &QTcpSocket::readyRead,    this, &FtpSegmentWorker::onDataReadyRead);
            connect(m_data, &QTcpSocket::disconnected, this, &FtpSegmentWorker::onDataFinished);
            m_data->connectToHost(hp->first, hp->second);

            // REST antes do RETR. Offset 0 também manda REST 0: inofensivo e
            // mantém um único caminho de código.
            m_step = Step::Rest;
            m_ch->sendCommand(QString("REST %1").arg(m_seg.current));
            return;
        }

        case Step::Rest:
            // REST recusado com offset > 0 (retomada): o servidor mudou de
            // comportamento desde o probe que gravou supportsRange. Não dá p/
            // retomar, mas dá p/ recomeçar do zero (spec §3.5) — restartRequired.
            // Com offset 0 (fallback single-connection ou primeiro segmento), o
            // REST é um no-op: sua recusa (502 do servidor sem REST) é inofensiva
            // — RETR a partir do início entrega exatamente a fatia esperada.
            if (code != 350 && m_seg.current > 0) { emit restartRequired(m_seg.index); return; }
            m_step = Step::Retr;
            m_ch->sendCommand("RETR " + m_url.path());
            return;

        case Step::Retr:
            if (code != 150 && code != 125) {
                if (code == 550) {
                    emit failed(m_seg.index, QString("RETR: %1 %2").arg(code).arg(text),
                                FailureKind::Fatal);
                    return;
                }
                scheduleRetry(QString("RETR: %1 %2").arg(code).arg(text));
                return;
            }
            m_step = Step::Transferring;
            armIdleTimer(m_cfg.idleTimeoutMs);
            return;

        case Step::Transferring:
            // 226 = confirmação de controle de transferência completa. NÃO
            // finalizamos por aqui: o 226 chega pelo socket de CONTROLE e pode
            // ultrapassar os últimos bytes ainda em trânsito no socket de DADOS
            // (TCP não ordena entre sockets distintos). Finalizar aqui abortaria
            // o socket de dados antes de drená-lo -> arquivo truncado/zerado. O
            // fim de arquivo confiável é o EOF do socket de dados (onDataFinished);
            // segmentos com end terminam no corte (onDataReadyRead).
            return;
    }
}

void FtpSegmentWorker::onDataReadyRead() {
    if (m_stopped || m_finished || !m_data) return;
    armIdleTimer(m_cfg.idleTimeoutMs);

    const qint64 avail = m_data->bytesAvailable();
    if (avail <= 0) return;

    // O CORTE (spec §3.3): o servidor manda do REST até o fim do arquivo — não
    // sabe parar num byte. Quem corta somos nós. Clampar ANTES do take(): o
    // socket de dados frequentemente bufferiza além do 'end' perto da cauda
    // do segmento, e o limiter é GLOBAL (compartilhado por todos os workers
    // HTTP+FTP) — pedir tokens para bytes que nunca serão lidos rouba banda
    // de outras transferências concorrentes.
    qint64 want = avail;
    if (m_seg.end >= 0) {
        const qint64 room = m_seg.end - m_seg.current + 1;
        if (room <= 0) { finishSegment(); return; }
        want = qMin(want, room);
    }

    qint64 grant = want;
    if (m_limiter) {
        grant = m_limiter->take(want, QDateTime::currentMSecsSinceEpoch());
        if (grant <= 0) { scheduleDrain(); return; }   // sem tokens: tentar de novo em breve
    }

    const QByteArray chunk = m_data->read(grant);
    if (chunk.isEmpty()) return;

    m_file->seek(m_seg.current);
    const qint64 written = m_file->write(chunk);
    if (written < 0) {
        emit failed(m_seg.index, "write error: " + m_file->errorString(), FailureKind::Fatal);
        m_finished = true;
        teardown();
        return;
    }
    m_seg.current += written;
    emit progressed(m_seg.index, m_seg.current);

    if (m_seg.end >= 0 && m_seg.current > m_seg.end) { finishSegment(); return; }
    if (m_data->bytesAvailable() > 0) scheduleDrain();   // restou dado sob throttle
}

void FtpSegmentWorker::scheduleDrain() {
    if (m_stopped || m_finished) return;
    if (!m_drainTimer) {
        m_drainTimer = new QTimer(this);
        m_drainTimer->setSingleShot(true);
        connect(m_drainTimer, &QTimer::timeout, this, &FtpSegmentWorker::onDataReadyRead);
    }
    if (!m_drainTimer->isActive()) m_drainTimer->start(20);   // ~20ms até repor tokens
}

void FtpSegmentWorker::onDataFinished() {
    if (m_stopped || m_finished) return;
    if (m_drainTimer) m_drainTimer->stop();   // não deixar um drain pendente reentrar depois do teardown()

    // Drena TUDO que ainda houver no buffer ANTES de decidir, bypassando o
    // limiter. O `disconnected` pode chegar com o último(s) chunk(s) ainda por
    // ler (readyRead pendente/coalescido), e finishSegment()/scheduleRetry()
    // fazem teardown() -> abort(), que descarta o buffer não-lido. Sem este
    // drain, um EOF que chega junto com os dados perde bytes (corrida rara,
    // dependente de carga) — o arquivo fica só com o preenchimento zerado.
    // Sob throttle, um único onDataReadyRead() não basta (pode conceder menos
    // do que o disponível); a transferência já terminou, então os bytes já
    // chegaram pelo fio — throttlá-los mais só produziria um short-read
    // espúrio (mesmo raciocínio do SegmentWorker::onFinished() do HTTP).
    if (m_data && m_data->bytesAvailable() > 0) {
        qint64 avail = m_data->bytesAvailable();
        if (m_seg.end >= 0) {
            const qint64 room = m_seg.end - m_seg.current + 1;
            if (room <= 0) { finishSegment(); return; }
            avail = qMin(avail, room);
        }
        const QByteArray rest = m_data->read(avail);
        if (!rest.isEmpty()) {
            m_file->seek(m_seg.current);
            const qint64 written = m_file->write(rest);
            if (written < 0) {
                emit failed(m_seg.index, "write error: " + m_file->errorString(), FailureKind::Fatal);
                m_finished = true;
                teardown();
                return;
            }
            m_seg.current += written;
            emit progressed(m_seg.index, m_seg.current);
        }
        if (m_seg.end >= 0 && m_seg.current > m_seg.end) { finishSegment(); return; }
    }

    // Socket de dados fechou. Fallback: isso é EOF = sucesso. Segmento com end:
    // fechou antes de completarmos = leitura curta, tenta de novo do offset atual.
    if (m_seg.end < 0) { finishSegment(); return; }
    if (m_seg.current <= m_seg.end) scheduleRetry("short read");
}

void FtpSegmentWorker::finishSegment() {
    if (m_finished) return;
    m_finished = true;
    m_file->flush();
    if (m_seg.end < 0) m_seg.end = m_seg.current - 1;   // fallback: end vira o EOF
    teardown();
    emit completed(m_seg.index);
}

void FtpSegmentWorker::onControlFailed(const QString& error, FtpErrorClass cls) {
    if (m_stopped || m_finished) return;
    switch (cls) {
        case FtpErrorClass::Transient: scheduleRetry(error); return;
        case FtpErrorClass::Auth:
            // 530 no resume: o probe não rodou (m_probed do .meta), então é por
            // aqui que o pedido de credenciais nasce (spec §3.6 (b)).
            emit failed(m_seg.index, error, FailureKind::AuthRequired);
            m_finished = true;
            teardown();
            return;
        case FtpErrorClass::Fatal:
            emit failed(m_seg.index, error, FailureKind::Fatal);
            m_finished = true;
            teardown();
            return;
    }
}

void FtpSegmentWorker::armIdleTimer(int ms) {
    if (!m_idleTimer) {
        m_idleTimer = new QTimer(this);
        m_idleTimer->setSingleShot(true);
        connect(m_idleTimer, &QTimer::timeout, this, &FtpSegmentWorker::onTimeout);
    }
    m_idleTimer->start(ms);
}

void FtpSegmentWorker::onTimeout() {
    if (m_stopped || m_finished) return;
    scheduleRetry("timeout");
}

void FtpSegmentWorker::scheduleRetry(const QString& why) {
    Q_UNUSED(why);
    teardown();
    if (m_attempt >= m_cfg.maxSegmentRetries) {
        emit failed(m_seg.index, "retries exhausted", FailureKind::Fatal);
        m_finished = true;
        return;
    }
    ++m_attempt;
    const int delay = m_cfg.retryBackoffBaseMs * (1 << (m_attempt - 1));
    if (!m_retryTimer) {
        m_retryTimer = new QTimer(this);
        m_retryTimer->setSingleShot(true);
        connect(m_retryTimer, &QTimer::timeout, this, &FtpSegmentWorker::openAttempt);
    }
    m_retryTimer->start(delay);
}

// Derruba a tentativa atual sem tocar em m_seg (o offset avançado é o ponto de
// partida do próximo retry). Desconecta antes de abortar: abortar dispara
// sinais que chegariam depois, encontrando estado já reciclado — mesma
// disciplina de SegmentWorker::onTimeout (SegmentWorker.cpp:172).
void FtpSegmentWorker::teardown() {
    if (m_idleTimer) m_idleTimer->stop();
    if (m_drainTimer) m_drainTimer->stop();
    if (m_data) {
        m_data->disconnect(this);
        m_data->abort();
        m_data->deleteLater();
        m_data = nullptr;
    }
    if (m_ch) {
        m_ch->disconnect(this);
        m_ch->close();
        m_ch->deleteLater();
        m_ch = nullptr;
    }
}

void FtpSegmentWorker::stop() {
    m_stopped = true;
    if (m_retryTimer) m_retryTimer->stop();
    teardown();
}
