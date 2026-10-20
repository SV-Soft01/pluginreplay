#pragma once

#include "camera-pipeline.hpp"
#include "replay-types.hpp"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace SVReplay {

using SaveResultListener = std::function<void(const SaveResult &result)>;

// Gestor central del plugin.
//
//  - Mantiene la lista de cámaras elegidas ("deseadas") y un CameraPipeline por cada una.
//  - Un hilo de supervisión (1 Hz) arranca, reintenta y repara los pipelines, de modo que
//    la interfaz de OBS nunca se bloquea esperando a un codificador.
//  - Coordina el guardado simultáneo de todas las cámaras (misma jugada, misma marca de tiempo).
class MultiCamManager {
public:
    static MultiCamManager &Instance();

    // Ciclo de vida
    void Initialize(const std::string &configFilePath);
    void Shutdown();

    // Cámaras (por nombre de fuente o escena de OBS)
    void SetCameraEnabled(const std::string &name, bool enabled);
    bool IsCameraEnabled(const std::string &name) const;
    std::vector<std::string> GetEnabledCameras() const;

    // Reemplaza la lista de cámaras (p. ej. al cargar una colección de escenas) sin arrancarlas.
    void SetEnabledCameras(const std::vector<std::string> &names);
    // Crea/elimina pipelines para que coincidan con la lista de cámaras.
    void ApplyEnabledCameras();
    // Detiene y libera todos los pipelines de forma síncrona (la lista de cámaras se conserva).
    void StopAllPipelines();

    // Ajustes globales
    ReplaySettings GetSettings() const;
    void UpdateSettings(const ReplaySettings &settings);

    // Estado para la interfaz
    std::vector<CameraStatus> GetStatuses() const;
    bool IsSaving() const;

    // Guardado de repeticiones
    bool TriggerSaveReplay(std::string *outError = nullptr);
    void SetResultListener(SaveResultListener listener);

private:
    MultiCamManager() = default;
    ~MultiCamManager();
    MultiCamManager(const MultiCamManager &) = delete;
    MultiCamManager &operator=(const MultiCamManager &) = delete;

    struct SaveSession {
        bool active = false;
        uint32_t playId = 0;
        std::string timestamp;
        std::string prefix;
        std::set<std::string> pending;
        SaveResult result;
        uint64_t startedNs = 0;
    };

    // Hilo de supervisión
    void WorkerLoop();
    void TickPipelines();
    void CheckSessionTimeout();

    // Cámaras
    void ReconcileLocked();
    void RefreshStatusCache();

    // Guardado
    void OnClipSaved(const std::string &cameraName, const std::string &savedPath);
    void FinalizeClip(const std::string &cameraName, const std::string &savedPath, uint32_t playId,
                      const std::string &timestamp, const std::string &prefix);
    void CompleteClip(const std::string &cameraName, bool ok, const std::string &finalPath,
                      const std::string &error);
    void FinishSession(SaveSession &session);

    // Configuración
    void LoadGlobalConfig();
    void SaveGlobalConfig();
    static std::string GenerateTimestampString();

    std::atomic<bool> m_initialized{false};
    std::string m_configPath;

    mutable std::mutex m_mutex; // m_settings, m_desired, m_pipelines, m_graveyard
    ReplaySettings m_settings;
    std::set<std::string> m_desired;
    std::map<std::string, std::shared_ptr<CameraPipeline>> m_pipelines;
    std::vector<std::shared_ptr<CameraPipeline>> m_graveyard;

    mutable std::mutex m_statusMutex;
    std::vector<CameraStatus> m_statusCache;

    mutable std::mutex m_sessionMutex;
    SaveSession m_session;

    std::mutex m_listenerMutex;
    SaveResultListener m_listener;

    std::atomic<uint32_t> m_playCounter{1};
    std::atomic<int> m_pendingWorkers{0};

    std::thread m_worker;
    std::mutex m_workerMutex;
    std::condition_variable m_workerCv;
    bool m_workerStop = false;
};

} // namespace SVReplay