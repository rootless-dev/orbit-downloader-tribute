#include "BootOptions.h"

bool shouldStartHidden(const QStringList& args) {
    return args.contains(QLatin1String(kBackgroundFlag));
}
