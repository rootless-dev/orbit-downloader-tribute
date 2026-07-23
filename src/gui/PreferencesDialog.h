#pragma once
#include "Settings.h"
#include <QDialog>
#include <QTime>
class QSpinBox;
class QLineEdit;
class QComboBox;
class QCheckBox;
class QTimeEdit;
class QListWidget;
class QStackedWidget;

class PreferencesDialog : public QDialog {
    Q_OBJECT
public:
    enum class Category {
        General, Location, DownloadsConnection, Limits, Appearance,
        Monitoring, Scheduler, P2PNetwork, Proxy, Others
    };

    explicit PreferencesDialog(const AppSettings& current, QWidget* parent = nullptr);
    AppSettings result() const;
    void setInitialCategory(Category c);

    // Test hooks (no exec() needed):
    void setConcurrentForTest(int n);
    void setMaxKBpsForTest(int kbps);
    void setUserAgentCustomForTest(const QString& ua);
    void setBrowserEnabledForTest(bool on);
    void setStartAtLoginForTest(bool on);
    void setSchedulerEnabledForTest(bool on);
    void setSchedulerStartForTest(QTime t);
    void setSchedulerStopForTest(QTime t);
    void setSchedulerRecurrenceForTest(Recurrence r);
    void setSchedulerQuitForTest(bool on);

private:
    void loadFromSettings(const AppSettings& s);   // (re)apply values onto existing widgets

    AppSettings m_base;                 // preserves non-edited fields
    // Navigation
    QListWidget*    m_categoryList = nullptr;
    QStackedWidget* m_stack        = nullptr;
    // Appearance
    QComboBox* m_theme      = nullptr;
    // General / Location / Downloads / Limits
    QSpinBox*  m_concurrent = nullptr;
    QSpinBox*  m_segments   = nullptr;
    QSpinBox*  m_maxKBps    = nullptr;  // 0 = unlimited
    QLineEdit* m_dir        = nullptr;
    QComboBox* m_clipMode   = nullptr;
    QComboBox* m_uaPreset   = nullptr;
    QLineEdit* m_uaCustom   = nullptr;
    QCheckBox* m_startAtLogin = nullptr;
    QSpinBox*  m_connectMs  = nullptr;
    QSpinBox*  m_idleMs     = nullptr;
    QSpinBox*  m_retries    = nullptr;
    QSpinBox*  m_backoffMs  = nullptr;
    QSpinBox*  m_minSegKB   = nullptr;
    QSpinBox*  m_throttleMs = nullptr;
    // Monitoring (browser bridge)
    QCheckBox* m_brEnabled  = nullptr;
    QSpinBox*  m_brPort     = nullptr;
    QLineEdit* m_brToken    = nullptr;   // read-only
    QString    m_brTokenValue;
    // Scheduler
    QCheckBox* m_schedEnabled    = nullptr;
    QTimeEdit* m_schedStart      = nullptr;
    QTimeEdit* m_schedStop       = nullptr;
    QComboBox* m_schedRecurrence = nullptr;
    QCheckBox* m_schedQuit       = nullptr;
};
