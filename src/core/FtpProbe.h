#pragma once
#include "Transport.h"
class FtpControlChannel;

class FtpProbe : public Probe {
    Q_OBJECT
public:
    explicit FtpProbe(const EngineConfig& cfg, QObject* parent = nullptr);
    void start(const QUrl& url, const Credentials& creds,
              const HeaderList& extraHeaders) override;
private:
    enum class Step { Size, Mdtm, RestProbe, RestReset };
    void onLoggedIn();
    void onReply(int code, const QString& text);
    void finishOk();
    // O tratamento de `failed` do canal é uma lambda no .cpp (precisa do tipo
    // FtpErrorClass, que só o .cpp inclui) — por isso não há slot p/ ele aqui.

    EngineConfig       m_cfg;
    FtpControlChannel* m_ch = nullptr;
    QUrl               m_url;
    ProbeResult        m_r;
    Step               m_step = Step::Size;
    bool               m_done = false;
};
