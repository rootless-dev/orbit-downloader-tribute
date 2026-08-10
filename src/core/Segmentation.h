#pragma once
#include "DownloadTypes.h"

QVector<Segment> computeSegments(qint64 totalBytes, bool supportsRange,
                                 int segmentCount, qint64 minSegSize);

// Divisão dinâmica (work splitting). A segmentação inicial é estática: quando
// um segmento termina, seu worker morre e a conexão é perdida — no fim do
// download sobra um único segmento longo baixando sozinho (a "cauda"). Para
// manter as N conexões vivas até o último byte, o segmento com MAIOR resto é
// cortado ao meio e a metade final vira um segmento novo, entregue ao worker
// que acabou de ficar livre.
struct SplitPlan {
    bool   ok      = false;
    int    index   = -1;   // segmento a encolher (fica com [start, splitAt-1])
    qint64 splitAt = 0;    // primeiro byte da metade doada
};

// Escolhe o segmento a dividir: o de maior resto (end - current + 1), empate
// resolvido pelo menor índice. Segmentos completos e fallback (end < 0, sem
// Range: não há como pedir uma faixa) nunca são candidatos. Só divide se o
// resto couber DUAS metades de minSplitBytes — abaixo disso a divisão só
// somaria handshake/latência de conexão a uma cauda que já está acabando.
SplitPlan planSplit(const QVector<Segment>& segments, qint64 minSplitBytes);
