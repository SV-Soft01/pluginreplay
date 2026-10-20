#include "multicam-dock.hpp"
#include "../multicam-manager.hpp"

#include <obs.h>
#include <obs-frontend-api.h>

#include <QApplication>
#include <QFileDialog>
#include <QHeaderView>
#include <QMessageBox>

namespace SVReplay {

MultiCamDock::MultiCamDock(QWidget *parent)
    : QWidget(parent)
{
    setObjectName("SVMultiCamReplayDock");

    SetupUI();
    LoadSettingsToUI();
    RefreshAvailableSources();

    MultiCamManager::Instance().SetResultListener([this](const SaveResult &result) {
        OnSaveResult(result);
    });

    m_refreshTimer = new QTimer(this);
    connect(m_refreshTimer, &QTimer::timeout, this, &MultiCamDock::OnPeriodicStatusUpdate);
    m_refreshTimer->start(500);
}

MultiCamDock::~MultiCamDock()
{
    if (m_refreshTimer) {
        m_refreshTimer->stop();
    }
}

void MultiCamDock::SetupUI()
{
    auto *mainContainer = new QWidget(this);
    auto *mainLayout = new QVBoxLayout(mainContainer);
    mainLayout->setContentsMargins(10, 10, 10, 10);
    mainLayout->setSpacing(10);

    // 1. Grupo: Selección de Fuentes
    auto *sourcesGroup = new QGroupBox("Fuentes y Cámaras Disponibles en OBS", mainContainer);
    auto *sourcesLayout = new QVBoxLayout(sourcesGroup);

    m_sourcesListWidget = new QListWidget(sourcesGroup);
    m_sourcesListWidget->setSelectionMode(QAbstractItemView::NoSelection);
    sourcesLayout->addWidget(m_sourcesListWidget);

    m_refreshSourcesBtn = new QPushButton("Escanear Fuentes de OBS", sourcesGroup);
    connect(m_refreshSourcesBtn, &QPushButton::clicked, this, &MultiCamDock::RefreshAvailableSources);
    sourcesLayout->addWidget(m_refreshSourcesBtn);

    mainLayout->addWidget(sourcesGroup);

    // 2. Grupo: Configuración del Búfer
    auto *bufferGroup = new QGroupBox("Configuración del Búfer Circular (H.264 Comprimido)", mainContainer);
    auto *bufferLayout = new QVBoxLayout(bufferGroup);

    auto *durationHBox = new QHBoxLayout();
    durationHBox->addWidget(new QLabel("Duración (segundos):", bufferGroup));

    m_durationSpinBox = new QSpinBox(bufferGroup);
    m_durationSpinBox->setRange(1, 120);
    m_durationSpinBox->setValue(10);
    durationHBox->addWidget(m_durationSpinBox);

    m_durationSlider = new QSlider(Qt::Horizontal, bufferGroup);
    m_durationSlider->setRange(1, 120);
    m_durationSlider->setValue(10);
    durationHBox->addWidget(m_durationSlider);

    connect(m_durationSpinBox, QOverload<int>::of(&QSpinBox::valueChanged),
            m_durationSlider, &QSlider::setValue);
    connect(m_durationSlider, &QSlider::valueChanged,
            m_durationSpinBox, &QSpinBox::setValue);
    connect(m_durationSpinBox, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &MultiCamDock::OnDurationChanged);

    bufferLayout->addLayout(durationHBox);

    m_memoryEstimateLabel = new QLabel("Memoria estimada en RAM: 0 MB", bufferGroup);
    m_memoryEstimateLabel->setStyleSheet("font-weight: bold; color: #3498db;");
    bufferLayout->addWidget(m_memoryEstimateLabel);

    mainLayout->addWidget(bufferGroup);

    // 3. Grupo: Guardado y Nomenclatura
    auto *saveGroup = new QGroupBox("Destino y Nombres de Archivo", mainContainer);
    auto *saveLayout = new QVBoxLayout(saveGroup);

    auto *dirHBox = new QHBoxLayout();
    m_dirLineEdit = new QLineEdit(saveGroup);
    m_dirLineEdit->setPlaceholderText("Carpeta de destino para los clips...");
    dirHBox->addWidget(m_dirLineEdit);

    m_browseDirBtn = new QPushButton("Examinar...", saveGroup);
    connect(m_browseDirBtn, &QPushButton::clicked, this, &MultiCamDock::OnBrowseDirectory);
    dirHBox->addWidget(m_browseDirBtn);
    saveLayout->addLayout(dirHBox);

    auto *prefixHBox = new QHBoxLayout();
    prefixHBox->addWidget(new QLabel("Prefijo de archivo:", saveGroup));
    m_prefixLineEdit = new QLineEdit("replay", saveGroup);
    connect(m_prefixLineEdit, &QLineEdit::textChanged, this, &MultiCamDock::OnPrefixChanged);
    prefixHBox->addWidget(m_prefixLineEdit);
    saveLayout->addLayout(prefixHBox);

    mainLayout->addWidget(saveGroup);

    // 4. Grupo: Monitor de Estado en Tiempo Real
    auto *statusGroup = new QGroupBox("Monitor de Estado Multicámara", mainContainer);
    auto *statusLayout = new QVBoxLayout(statusGroup);

    m_statusTableWidget = new QTableWidget(0, 4, statusGroup);
    QStringList headers;
    headers << "Fuente" << "Estado" << "Fotogramas" << "Búfer (s)";
    m_statusTableWidget->setHorizontalHeaderLabels(headers);
    m_statusTableWidget->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_statusTableWidget->setEditTriggers(QAbstractItemView::NoEditTriggers);
    statusLayout->addWidget(m_statusTableWidget);

    mainLayout->addWidget(statusGroup);

    // 5. Controles de Disparo y Atajo
    auto *actionLayout = new QVBoxLayout();

    auto *hotkeyNotice = new QLabel(
        "Atajo configurable en: <b>Ajustes de OBS -> Atajos -> SV MultiCam Replay</b>",
        mainContainer
    );
    hotkeyNotice->setStyleSheet("color: #7f8c8d; font-size: 11px;");
    actionLayout->addWidget(hotkeyNotice);

    m_saveReplayBtn = new QPushButton("GUARDAR REPETICIÓN AHORA (Prueba)", mainContainer);
    m_saveReplayBtn->setStyleSheet(
        "QPushButton { background-color: #27ae60; color: white; font-weight: bold; font-size: 13px; padding: 10px; border-radius: 4px; }"
        "QPushButton:hover { background-color: #2ecc71; }"
        "QPushButton:pressed { background-color: #1e8449; }"
        "QPushButton:disabled { background-color: #7f8c8d; }"
    );
    connect(m_saveReplayBtn, &QPushButton::clicked, this, &MultiCamDock::OnTriggerSaveTestReplay);
    actionLayout->addWidget(m_saveReplayBtn);

    m_saveProgressBar = new QProgressBar(mainContainer);
    m_saveProgressBar->setRange(0, 0); // Modo indeterminado durante el guardado
    m_saveProgressBar->setVisible(false);
    actionLayout->addWidget(m_saveProgressBar);

    m_statusMessageLabel = new QLabel("", mainContainer);
    m_statusMessageLabel->setAlignment(Qt::AlignCenter);
    m_statusMessageLabel->setWordWrap(true);
    actionLayout->addWidget(m_statusMessageLabel);

    mainLayout->addLayout(actionLayout);

    auto *outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(0, 0, 0, 0);
    outerLayout->addWidget(mainContainer);

    connect(m_sourcesListWidget, &QListWidget::itemChanged, this, &MultiCamDock::OnSourceItemChanged);
}

void MultiCamDock::LoadSettingsToUI()
{
    auto settings = MultiCamManager::Instance().GetSettings();
    m_durationSpinBox->setValue(settings.bufferSeconds);
    m_durationSlider->setValue(settings.bufferSeconds);
    m_dirLineEdit->setText(QString::fromStdString(settings.outputDirectory));
    m_prefixLineEdit->setText(QString::fromStdString(settings.filePrefix));
    UpdateMemoryEstimate();
}

void MultiCamDock::RefreshAvailableSources()
{
    m_sourcesListWidget->blockSignals(true);
    m_sourcesListWidget->clear();

    struct EnumContext {
        QListWidget *listWidget;
    } context;
    context.listWidget = m_sourcesListWidget;

    obs_enum_sources([](void *param, obs_source_t *source) -> bool {
        if (!source) return true;

        uint32_t outputFlags = obs_source_get_output_flags(source);
        if ((outputFlags & OBS_SOURCE_VIDEO) == 0) {
            return true;
        }

        const char *name = obs_source_get_name(source);
        if (!name || strlen(name) == 0) {
            return true;
        }

        auto *ctx = static_cast<EnumContext*>(param);
        auto *item = new QListWidgetItem(QString::fromUtf8(name), ctx->listWidget);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);

        bool isEnabled = MultiCamManager::Instance().IsCameraEnabled(name);
        item->setCheckState(isEnabled ? Qt::Checked : Qt::Unchecked);

        return true;
    }, &context);

    m_sourcesListWidget->blockSignals(false);
    UpdateMemoryEstimate();
}

void MultiCamDock::OnSourceItemChanged(QListWidgetItem *item)
{
    if (!item) return;
    std::string sourceName = item->text().toStdString();
    bool checked = (item->checkState() == Qt::Checked);

    MultiCamManager::Instance().SetCameraEnabled(sourceName, checked);
    UpdateMemoryEstimate();
}

void MultiCamDock::OnDurationChanged(int value)
{
    auto s = MultiCamManager::Instance().GetSettings();
    s.bufferSeconds = value;
    MultiCamManager::Instance().UpdateSettings(s);
    UpdateMemoryEstimate();
}

void MultiCamDock::OnBrowseDirectory()
{
    QString current = m_dirLineEdit->text();
    QString selected = QFileDialog::getExistingDirectory(
        this,
        "Seleccionar carpeta de destino para las repeticiones",
        current.isEmpty() ? QDir::homePath() : current
    );

    if (!selected.isEmpty()) {
        m_dirLineEdit->setText(selected);
        auto s = MultiCamManager::Instance().GetSettings();
        s.outputDirectory = selected.toStdString();
        MultiCamManager::Instance().UpdateSettings(s);
    }
}

void MultiCamDock::OnPrefixChanged(const QString &text)
{
    auto s = MultiCamManager::Instance().GetSettings();
    s.filePrefix = text.toStdString();
    MultiCamManager::Instance().UpdateSettings(s);
}

void MultiCamDock::UpdateMemoryEstimate()
{
    auto cameras = MultiCamManager::Instance().GetEnabledCameras();
    int duration = m_durationSpinBox->value();

    // Estimación empírica para video comprimido H.264 (~1.5 MB por segundo por cámara)
    double estimatedMbPerCamera = (1.5 * duration);
    double totalEstimatedMb = cameras.size() * estimatedMbPerCamera;

    m_memoryEstimateLabel->setText(
        QString("Memoria estimada en RAM: ~%1 MB (%2 cámaras activas)")
            .arg(static_cast<int>(totalEstimatedMb))
            .arg(cameras.size())
    );
}

void MultiCamDock::OnTriggerSaveTestReplay()
{
    std::string error;
    bool started = MultiCamManager::Instance().TriggerSaveReplay(&error);

    if (!started) {
        m_saveReplayBtn->setEnabled(true);
        m_saveProgressBar->setVisible(false);
        m_statusMessageLabel->setStyleSheet("color: #c0392b; font-weight: bold;");
        m_statusMessageLabel->setText(QString::fromStdString(error.empty() ? "No se pudo iniciar el guardado." : error));
    } else {
        m_saveReplayBtn->setEnabled(false);
        m_saveProgressBar->setVisible(true);
        m_statusMessageLabel->setStyleSheet("color: #e67e22; font-weight: bold;");
        m_statusMessageLabel->setText("Procesando repetición y empaquetando clips MP4...");
    }
}

void MultiCamDock::OnSaveResult(const SaveResult &result)
{
    QMetaObject::invokeMethod(this, [this, result]() {
        m_saveReplayBtn->setEnabled(true);
        m_saveProgressBar->setVisible(false);

        if (result.success) {
            m_statusMessageLabel->setStyleSheet("color: #27ae60; font-weight: bold;");
            m_statusMessageLabel->setText(
                QString("¡Repetición #%1 guardada con éxito! (%2 clips sincronizados)")
                    .arg(result.playId, 4, 10, QChar('0'))
                    .arg(result.totalClips)
            );
        } else {
            m_statusMessageLabel->setStyleSheet("color: #c0392b; font-weight: bold;");
            QString err = result.errors.empty() ? "Error al guardar los clips." : QString::fromStdString(result.errors[0]);
            m_statusMessageLabel->setText(err);
        }
    });
}

void MultiCamDock::OnPeriodicStatusUpdate()
{
    auto statuses = MultiCamManager::Instance().GetStatuses();
    m_statusTableWidget->setRowCount(static_cast<int>(statuses.size()));

    for (int i = 0; i < static_cast<int>(statuses.size()); ++i) {
        const auto &st = statuses[i];
        QString stateStr;
        switch (st.state) {
        case PipelineState::Running:
            stateStr = "Búfer Activo";
            break;
        case PipelineState::Starting:
            stateStr = "Iniciando...";
            break;
        case PipelineState::WaitingForSource:
            stateStr = QString::fromStdString(st.detail.empty() ? "Esperando Fuente" : st.detail);
            break;
        case PipelineState::Error:
            stateStr = QString::fromStdString(st.detail.empty() ? "Error" : st.detail);
            break;
        case PipelineState::Stopped:
        default:
            stateStr = "Detenido";
            break;
        }

        size_t approxFrames = static_cast<size_t>(st.bufferedSeconds * 60.0);

        auto *nameItem = new QTableWidgetItem(QString::fromStdString(st.name));
        auto *statusItem = new QTableWidgetItem(stateStr);
        auto *framesItem = new QTableWidgetItem(QString::number(approxFrames));
        auto *secondsItem = new QTableWidgetItem(QString::number(st.bufferedSeconds, 'f', 1) + " s");

        nameItem->setTextAlignment(Qt::AlignCenter);
        statusItem->setTextAlignment(Qt::AlignCenter);
        framesItem->setTextAlignment(Qt::AlignCenter);
        secondsItem->setTextAlignment(Qt::AlignCenter);

        if (st.state == PipelineState::Running) {
            statusItem->setForeground(QBrush(QColor("#27ae60")));
        } else if (st.state == PipelineState::Error) {
            statusItem->setForeground(QBrush(QColor("#c0392b")));
        } else if (st.state == PipelineState::WaitingForSource) {
            statusItem->setForeground(QBrush(QColor("#e67e22")));
        }

        m_statusTableWidget->setItem(i, 0, nameItem);
        m_statusTableWidget->setItem(i, 1, statusItem);
        m_statusTableWidget->setItem(i, 2, framesItem);
        m_statusTableWidget->setItem(i, 3, secondsItem);
    }

    bool saving = MultiCamManager::Instance().IsSaving();
    if (!saving && !m_saveReplayBtn->isEnabled()) {
        m_saveReplayBtn->setEnabled(true);
        m_saveProgressBar->setVisible(false);
    }
}

} // namespace SVReplay
