#include "HttpProbe.h"
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include "ContentDisposition.h"

HttpProbe::HttpProbe(QNetworkAccessManager* nam, QByteArray userAgent, QObject* parent)
    : Probe(parent), m_nam(nam), m_userAgent(std::move(userAgent)) {
    qRegisterMetaType<ProbeResult>("ProbeResult");
}

void HttpProbe::start(const QUrl& url, const Credentials& creds,
                      const HeaderList& extraHeaders) {
    Q_UNUSED(creds);   // HTTP basic-auth não está no escopo da Fase 3
    QNetworkRequest req(url);
    req.setRawHeader("Range", "bytes=0-0");
    req.setRawHeader("User-Agent", m_userAgent);
    for (const auto& h : extraHeaders) req.setRawHeader(h.first, h.second);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    m_reply = m_nam->get(req);
    connect(m_reply, &QNetworkReply::metaDataChanged, this, &HttpProbe::onMetaDataChanged);
    connect(m_reply, &QNetworkReply::errorOccurred,   this, &HttpProbe::onErrorOccurred);
}

void HttpProbe::onMetaDataChanged() {
    if (m_done) return;
    const int status = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    // metaDataChanged dispara a cada hop quando o QNAM segue redirects
    // (RedirectPolicy definida no start). Ignore as respostas 3xx que ele vai
    // seguir e espere a final; se um redirect não puder ser seguido, o erro
    // chega por onErrorOccurred. Sem isto, um 302 intermediário (ex.: links
    // Google Takeout) era latcheado como resultado final e virava "HTTP 302".
    if (status == 301 || status == 302 || status == 303 ||
        status == 307 || status == 308)
        return;
    m_done = true;
    ProbeResult r;
    r.resolvedUrl  = m_reply->url();
    r.httpStatus   = status;
    r.contentType  = QString::fromUtf8(m_reply->rawHeader("Content-Type"));
    r.etag         = QString::fromUtf8(m_reply->rawHeader("ETag"));
    r.lastModified = QString::fromUtf8(m_reply->rawHeader("Last-Modified"));
    const QByteArray cd = m_reply->rawHeader("Content-Disposition");
    if (!cd.isEmpty())
        r.suggestedFileName = parseContentDisposition(QString::fromUtf8(cd));

    if (status == 206) {
        r.supportsRange = true;
        const QByteArray cr = m_reply->rawHeader("Content-Range");   // "bytes 0-0/12345"
        const int slash = cr.lastIndexOf('/');
        if (slash >= 0) r.totalBytes = cr.mid(slash + 1).trimmed().toLongLong();
        r.ok = true;
    } else if (status == 200) {
        r.supportsRange = false;
        const QVariant cl = m_reply->header(QNetworkRequest::ContentLengthHeader);
        r.totalBytes = cl.isValid() ? cl.toLongLong() : -1;
        r.ok = true;
    } else {
        r.ok = false;
        r.error = QString("HTTP %1").arg(status);
    }
    m_reply->abort();          // headers are enough; don't download the body
    m_reply->deleteLater();
    emit finished(r);
}

void HttpProbe::onErrorOccurred() {
    if (m_done) return;        // an abort() after success also fires this; guard it
    m_done = true;
    ProbeResult r;
    r.ok = false;
    r.error = m_reply->errorString();
    m_reply->deleteLater();
    emit finished(r);
}
