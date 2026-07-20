#pragma once
#include "Settings.h"
#include <QDialog>
class QSpinBox;
class QLineEdit;
class QComboBox;
class QCheckBox;

class PreferencesDialog : public QDialog {
    Q_OBJECT
public:
    explicit PreferencesDialog(const AppSettings& current, QWidget* parent = nullptr);
    AppSettings result() const;

    // Hooks de teste (sem depender de exec()):
    void setConcurrentForTest(int n);
    void setMaxKBpsForTest(int kbps);
    void setUserAgentCustomForTest(const QString& ua);
    void setBrowserEnabledForTest(bool on);

private:
    AppSettings m_base;                 // preserva campos não editados (timeouts avançados etc.)
    // Appearance
    QComboBox* m_theme      = nullptr;
    // General
    QSpinBox*  m_concurrent = nullptr;
    QSpinBox*  m_segments   = nullptr;
    QSpinBox*  m_maxKBps    = nullptr;  // 0 = ilimitado
    QLineEdit* m_dir        = nullptr;
    QComboBox* m_clipMode   = nullptr;
    QComboBox* m_uaPreset   = nullptr;
    QLineEdit* m_uaCustom   = nullptr;
    // Advanced
    QSpinBox*  m_connectMs  = nullptr;
    QSpinBox*  m_idleMs     = nullptr;
    QSpinBox*  m_retries    = nullptr;
    QSpinBox*  m_backoffMs  = nullptr;
    QSpinBox*  m_minSegKB   = nullptr;
    QSpinBox*  m_throttleMs = nullptr;
    // Browser
    QCheckBox* m_brEnabled  = nullptr;
    QSpinBox*  m_brPort     = nullptr;
    QLineEdit* m_brToken    = nullptr;   // read-only
    QString    m_brTokenValue;           // token atual (persistido no result())
};
