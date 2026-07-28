#pragma once
#include <QObject>
#include <QString>
#include <QUrl>
#include <optional>

enum class ClipboardMode { Off, Ask, Auto, Notify };

// Decide se um texto copiado merece virar oferta de download. Puro: sem
// clipboard, sem widgets, testável direto.
//   - só http/https/ftp
//   - ignora o que a própria app copiou (selfCopied)
//   - ignora repetição imediata da última URL oferecida (lastOffered)
//   - exige a URL limpa (prosa com link no meio não conta)
std::optional<QUrl> shouldOffer(const QString& text, const QUrl& lastOffered, bool selfCopied);

// Task 16: mesma ideia, mas p/ links magnet:. Não reusa shouldOffer() porque
// magnet: não é um esquema hierárquico (sem host) - isDownloadableScheme()
// sempre rejeitaria - e a validação de "é mesmo um magnet" é via
// MagnetUri::parse().isValid(), não via QUrl. Puro: sem clipboard, sem
// widgets, testável direto.
std::optional<QString> shouldOfferMagnet(const QString& text, const QString& lastOffered,
                                         bool selfCopied);

class ClipboardWatcher : public QObject {
    Q_OBJECT
public:
    explicit ClipboardWatcher(QObject* parent = nullptr);
    void setMode(ClipboardMode m) { m_mode = m; }
    ClipboardMode mode() const    { return m_mode; }
    void markSelfCopy();          // chame ANTES de a app escrever no clipboard
signals:
    void urlDetected(const QUrl& url);
    void magnetDetected(const QString& uri);   // Task 16
private:
    void onClipboardChanged();
    ClipboardMode m_mode = ClipboardMode::Off;   // padrão Off (spec §3.7)
    QUrl          m_lastOffered;
    QString       m_lastOfferedMagnet;            // Task 16
    bool          m_selfCopied = false;
};
