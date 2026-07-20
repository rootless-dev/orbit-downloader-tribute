#include "Theme.h"
#include <QApplication>
#include <QStyleHints>
#include <QPalette>

void applyTheme(ThemePref pref) {
    if (!qApp) return;
    Qt::ColorScheme scheme = Qt::ColorScheme::Unknown;   // System: segue o SO ao vivo
    switch (pref) {
        case ThemePref::Light: scheme = Qt::ColorScheme::Light; break;
        case ThemePref::Dark:  scheme = Qt::ColorScheme::Dark;  break;
        case ThemePref::System: default: scheme = Qt::ColorScheme::Unknown; break;
    }
    qApp->styleHints()->setColorScheme(scheme);
}

GridColors gridColors() {
    const QPalette pal = qApp ? qApp->palette() : QPalette();
    GridColors c;
    c.background = pal.color(QPalette::Base);   // fundo da área (segue o tema)
    c.pending    = pal.color(QPalette::Mid);    // neutro que segue o tema
    c.downloaded = QColor("#5b9bd5");           // azul Orbit (fixo)
    c.active     = QColor("#f7941e");           // laranja (fixo)
    c.error      = QColor("#ef4444");           // vermelho (fixo)
    return c;
}
