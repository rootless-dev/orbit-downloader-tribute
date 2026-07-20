#pragma once
#include <QList>
#include <QUrl>
class QMimeData;

// Esquemas que o motor sabe baixar (spec §3.7). Sem QtWidgets: testável
// headless e reusável pelo drop (Task 12) e pelo clipboard (Task 13).
bool isDownloadableScheme(const QUrl& u);

// Extrai as URLs baixáveis de um drop. Aceita text/uri-list e text/plain.
// Puro e sem QtWidgets: testável headless. Preserva a ordem e remove
// duplicatas. Lista vazia = drop deve ser rejeitado.
QList<QUrl> extractUrls(const QMimeData* mime);
