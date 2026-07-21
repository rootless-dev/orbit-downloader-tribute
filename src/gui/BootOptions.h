#pragma once
#include <QStringList>

// The CLI flag that tells the app to start hidden in the tray (passed only by
// the autostart LaunchAgent). Manual launches never pass it.
inline constexpr char kBackgroundFlag[] = "--background";

// True iff the app was asked to start hidden in the tray.
bool shouldStartHidden(const QStringList& args);
