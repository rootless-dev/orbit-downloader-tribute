#include "FtpProbe.h"
#include "FtpControlChannel.h"
#include "FtpReply.h"

FtpProbe::FtpProbe(const EngineConfig& cfg, QObject* parent)
    : Probe(parent), m_cfg(cfg) {
    qRegisterMetaType<ProbeResult>("ProbeResult");
}

void FtpProbe::start(const QUrl& url, const Credentials& creds,
                     const HeaderList& extraHeaders) {
    Q_UNUSED(extraHeaders);
    m_url = url;
    m_ch  = new FtpControlChannel(this);
    connect(m_ch, &FtpControlChannel::loggedIn,      this, &FtpProbe::onLoggedIn);
    connect(m_ch, &FtpControlChannel::replyReceived, this, &FtpProbe::onReply);
    connect(m_ch, &FtpControlChannel::failed, this,
            [this](const QString& e, FtpErrorClass cls) {
        if (m_done) return;
        m_done = true;
        m_r.ok           = false;
        m_r.error        = e;
        m_r.authRequired = (cls == FtpErrorClass::Auth);
        m_ch->close();
        emit finished(m_r);
    });
    m_ch->connectAndLogin(url, creds, m_cfg.connectTimeoutMs);
}

void FtpProbe::onLoggedIn() {
    m_r.resolvedUrl = m_url;
    m_step = Step::Size;
    m_ch->sendCommand("SIZE " + m_url.path());
}

void FtpProbe::onReply(int code, const QString& text) {
    if (m_done) return;
    switch (m_step) {
        case Step::Size:
            // 213 <n> = tamanho. Qualquer outra coisa (502/550) = tamanho
            // desconhecido; seguimos assim mesmo e o download cai em fallback.
            if (code == 213) {
                bool ok = false;
                const qint64 n = text.trimmed().toLongLong(&ok);
                if (ok) m_r.totalBytes = n;
            }
            m_step = Step::Mdtm;
            m_ch->sendCommand("MDTM " + m_url.path());
            return;

        case Step::Mdtm:
            // Guardamos a STRING crua (não o QDateTime): é ela que vai para o
            // .meta como validador e que o worker vai comparar (spec §3.5).
            if (code == 213 && parseMdtm(QString("213 %1").arg(text)).has_value())
                m_r.lastModified = text.trimmed();
            m_step = Step::RestProbe;
            m_ch->sendCommand("REST 1");
            return;

        case Step::RestProbe:
            // 350 = REST suportado. Sem tamanho conhecido não há o que
            // segmentar, então supportsRange exige os dois.
            m_r.supportsRange = (code == 350) && (m_r.totalBytes > 0);
            if (code == 350) {
                m_step = Step::RestReset;
                m_ch->sendCommand("REST 0");   // não deixa offset pendente na sessão
                return;
            }
            finishOk();
            return;

        case Step::RestReset:
            finishOk();
            return;
    }
}

void FtpProbe::finishOk() {
    if (m_done) return;
    m_done  = true;
    m_r.ok  = true;
    m_ch->sendCommand("QUIT");
    m_ch->close();
    emit finished(m_r);
}
