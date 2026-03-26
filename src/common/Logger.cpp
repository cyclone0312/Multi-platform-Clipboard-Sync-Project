#include "common/Logger.h"

#include <QLoggingCategory>

void setupLogging()
{
    qSetMessagePattern(QStringLiteral("[%{time yyyy-MM-dd hh:mm:ss.zzz}] %{type} %{message}"));
}
