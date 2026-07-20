#pragma once
#include <QString>

// Extrai um nome de arquivo seguro do VALOR de um header Content-Disposition.
// Regras: filename* (RFC 5987) vence filename; sanitização basename-only.
// Retorna string vazia quando não há filename utilizável (o chamador então
// usa o fallback derivado da URL).
QString parseContentDisposition(const QString& headerValue);
