#pragma once
#include "Settings.h"   // ThemePref
#include <QColor>

// Cores do progress grid: neutros seguem a QPalette do tema atual; as cores de
// identidade Orbit (downloaded/active/error) são fixas nos dois temas.
struct GridColors {
    QColor background;
    QColor downloaded;
    QColor active;
    QColor error;
    QColor pending;
};

// Aplica o tema à app inteira via QStyleHints::setColorScheme (Qt 6.8+).
// System => Unknown (segue o SO ao vivo); Light/Dark => forçado.
void applyTheme(ThemePref pref);

// Paleta do grid derivada do tema/QPalette atuais.
GridColors gridColors();
