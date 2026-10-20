#pragma once

#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QListWidget>
#include <QTableWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QSlider>
#include <QLabel>
#include <QProgressBar>
#include <QTimer>
#include <QGroupBox>

namespace SVReplay {

class MultiCamDock : public QWidget {
    Q_OBJECT

public:
    explicit MultiCamDock(QWidget *parent = nullptr);
    ~MultiCamDock() override;

    void RefreshAvailableSources();

private slots:
    void OnSourceItemChanged(QListWidgetItem *item);
    void OnDurationChanged(int value);
    void OnBrowseDirectory();
    void OnPrefixChanged(const QString &text);
    void OnTriggerSaveTestReplay();
    void OnPeriodicStatusUpdate();

private:
    void SetupUI();
    void LoadSettingsToUI();
    void UpdateMemoryEstimate();

    // Controles de UI
    QListWidget *m_sourcesListWidget = nullptr;
    QPushButton *m_refreshSourcesBtn = nullptr;

    QSpinBox *m_durationSpinBox = nullptr;
    QSlider *m_durationSlider = nullptr;

    QLineEdit *m_dirLineEdit = nullptr;
    QPushButton *m_browseDirBtn = nullptr;

    QLineEdit *m_prefixLineEdit = nullptr;

    QTableWidget *m_statusTableWidget = nullptr;
    QLabel *m_memoryEstimateLabel = nullptr;
    QLabel *m_statusMessageLabel = nullptr;

    QPushButton *m_saveReplayBtn = nullptr;
    QProgressBar *m_saveProgressBar = nullptr;

    QTimer *m_refreshTimer = nullptr;
};

} // namespace SVReplay
