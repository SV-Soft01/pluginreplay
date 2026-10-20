#pragma once

#include <obs.h>
#include <graphics/graphics.h>
#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <memory>
#include <cstdint>

namespace SVReplay {

// Representa un fotograma capturado o paquete de video
struct VideoFramePacket {
    uint64_t timestamp_ns = 0;   // Marca temporal de OBS (os_gettime_ns)
    uint32_t width = 0;
    uint32_t height = 0;
    bool is_keyframe = false;
    std::vector<uint8_t> data;   // Datos de imagen o paquete H.264
};

// Estado operativo del buffer de una fuente
enum class BufferStatus {
    Inactive,
    Initializing,
    Buffering,
    Saving,
    Error
};

class SourceBuffer {
public:
    explicit SourceBuffer(const std::string &sourceName);
    ~SourceBuffer();

    // Iniciar y detener la captura de la fuente
    bool StartCapture();
    void StopCapture();

    // Llamado dentro del render loop de gráficos de OBS (obs_add_main_render_callback)
    void RenderAndCapture(uint64_t current_time_ns);

    // Configuración de la duración del búfer en segundos
    void SetBufferDuration(int seconds);
    int GetBufferDuration() const { return m_durationSeconds; }

    // Estado e información
    const std::string& GetSourceName() const { return m_sourceName; }
    BufferStatus GetStatus() const { return m_status; }
    std::string GetStatusString() const;
    size_t GetEstimatedMemoryBytes() const;
    size_t GetBufferedFrameCount() const;
    double GetBufferedDurationSeconds() const;

    // Extrae una copia instantánea sincronizada de los fotogramas para guardar en MP4
    std::vector<VideoFramePacket> ExtractReplaySlice(uint64_t start_time_ns, uint64_t end_time_ns);

private:
    void ClearBufferInternal();
    void PruneOldFrames(uint64_t current_time_ns);

    std::string m_sourceName;
    obs_source_t *m_source = nullptr;
    gs_texrender_t *m_texrender = nullptr;
    gs_stagesurf_t *m_stagesurf = nullptr;

    int m_durationSeconds = 10;
    BufferStatus m_status = BufferStatus::Inactive;

    // Doble buffer para descarga de textura sin bloqueo GPU
    uint32_t m_currentWidth = 0;
    uint32_t m_currentHeight = 0;

    mutable std::mutex m_bufferMutex;
    std::deque<VideoFramePacket> m_ringBuffer;

    uint64_t m_lastFrameTimeNs = 0;
    size_t m_currentMemoryUsage = 0;
};

} // namespace SVReplay
