#pragma once

#include "replay-types.hpp"

#include <obs.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>

namespace SVReplay {

// Pipeline de UNA cámara:
//
//   fuente/escena -> obs_view (video propio) -> codificador H.264 -> salida
//   "replay_buffer" de OBS (búfer circular de paquetes ya comprimidos + muxer MP4).
//
// Todo el trabajo pesado (codificar, mantener el búfer, multiplexar a MP4) lo
// hace OBS con sus propios componentes, por lo que los archivos resultantes son
// MP4 estándar y el búfer ocupa solo unos MB en lugar de varios GB.
class CameraPipeline {
public:
    // Se invoca (desde un hilo de OBS) cuando OBS terminó de escribir un clip.
    using ClipSavedCallback = std::function<void(const std::string &cameraName, const std::string &savedPath)>;

    CameraPipeline(std::string sourceName, ClipSavedCallback onSaved);
    ~CameraPipeline();

    CameraPipeline(const CameraPipeline &) = delete;
    CameraPipeline &operator=(const CameraPipeline &) = delete;

    const std::string &Name() const { return m_name; }

    // Llamado periódicamente (cada ~1 s) por el hilo de supervisión del gestor.
    // Arranca, reintenta, detecta cambios de resolución y fuentes eliminadas.
    void Tick(const ReplaySettings &settings);

    // Pide un reinicio en el próximo Tick (por cambio de ajustes).
    void RequestRestart() { m_restartRequested = true; }

    // Detiene el pipeline de forma definitiva (no vuelve a arrancar solo).
    void Retire();

    // Pide a OBS guardar los últimos N segundos. Devuelve false si la cámara
    // no tiene un búfer activo en este momento.
    bool RequestSave();

    CameraStatus GetStatus() const;

private:
    struct SavedContext {
        CameraPipeline *owner = nullptr;
        obs_output_t *output = nullptr;
    };

    static void OnSavedSignal(void *param, calldata_t *cd);

    void StartLocked(const ReplaySettings &settings);
    void StopLocked();
    bool TryStartWithEncoderLocked(const char *encoderId, const ReplaySettings &settings, std::string &error);
    void TeardownEncodeChainLocked();
    void TeardownMediaLocked();
    void ReleaseSourceLocked();
    void SetStateLocked(PipelineState state, const std::string &detail);
    void ScheduleRetryLocked();
    void PublishSnapshotLocked();

    const std::string m_name;
    const ClipSavedCallback m_onSaved;
    const uint32_t m_uid;

    mutable std::mutex m_mutex; // protege todo el estado de abajo
    bool m_retired = false;
    std::atomic<bool> m_restartRequested{false};

    PipelineState m_state = PipelineState::Stopped;
    std::string m_detail;
    uint64_t m_nextRetryNs = 0;
    uint64_t m_startedNs = 0;

    obs_source_t *m_source = nullptr;
    bool m_wakeRefs = false;
    obs_view_t *m_view = nullptr;
    video_t *m_video = nullptr;
    obs_encoder_t *m_videoEncoder = nullptr;
    obs_encoder_t *m_audioEncoder = nullptr;
    obs_output_t *m_output = nullptr;
    SavedContext *m_savedContext = nullptr;

    uint32_t m_width = 0;
    uint32_t m_height = 0;
    int m_bufferSeconds = 0;
    int m_bitrateKbps = 0;
    std::string m_encoderId;

    mutable std::mutex m_snapshotMutex;
    CameraStatus m_snapshot;
};

} // namespace SVReplay
