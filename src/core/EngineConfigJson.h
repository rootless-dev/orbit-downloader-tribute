#pragma once
#include "DownloadTypes.h"
#include <QJsonObject>

QJsonObject  engineConfigToJson(const EngineConfig& c);
EngineConfig engineConfigFromJson(const QJsonObject& o, const EngineConfig& defaults);
