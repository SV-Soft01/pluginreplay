#include "source-buffer.hpp"
#include <util/platform.h>
#include <algorithm>

namespace SVReplay {

SourceBuffer::SourceBuffer(const std::string &sourceName)
    : m_sourceName(sourceName)
{
}

SourceBuffer::~SourceBuffer()
{
    StopCapture();
}

bool SourceBuffer::StartCapture()
{
    std::lock_guard<std::mutex> lock(m_bufferMutex);

    m_source = obs_get_source_by_name(m_sourceName.c_str());
    if (!m_source) {
        m_status = BufferStatus::Error;
        blog(LOG_WARNING, "[SV MultiCam] No se pudo encontrar la fuente '%s'", m_sourceName.c_str());
        return false;
    }

    // Mantener la fuente activa y emitiendo video independientemente de si esta en Programa
    obs_source_inc_showing(m_source);
    obs_source_inc_active(m_source);

    // Inicializar el renderizador de textura fuera de pantalla en el contexto gráfico
    obs_enter_graphics();
    m_texrender = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
    obs_leave_graphics();

    m_status = BufferStatus::Initializing;
    blog(LOG_INFO, "[SV MultiCam] Captura iniciada para la fuente '%s'", m_sourceName.c_str());
    return true;
}

void SourceBuffer::StopCapture()
{
    std::lock_guard<std::mutex> lock(m_bufferMutex);

    if (m_source) {
        obs_source_dec_showing(m_source);
        obs_source_dec_active(m_source);
        obs_source_release(m_source);
        m_source = nullptr;
    }

    obs_enter_graphics();
    if (m_stagesurf) {
        gs_stagesurface_destroy(m_stagesurf);
        m_stagesurf = nullptr;
    }
    if (m_texrender) {
        gs_texrender_destroy(m_texrender);
        m_texrender = nullptr;
    }
    obs_leave_graphics();

    m_currentWidth = 0;
    m_currentHeight = 0;
    ClearBufferInternal();
    m_status = BufferStatus::Inactive;
    blog(LOG_INFO, "[SV MultiCam] Captura detenida para la fuente '%s'", m_sourceName.c_str());
}

void SourceBuffer::RenderAndCapture(uint64_t current_time_ns)
{
    if (!m_source || !m_texrender) {
        return;
    }

    uint32_t width = obs_source_get_width(m_source);
    uint32_t height = obs_source_get_height(m_source);

    // Si la fuente aún no tiene dimensiones válidas (ej. webcam inicializando)
    if (width == 0 || height == 0) {
        return;
    }

    // Gestionar cambios de resolución dinámicos
    if (width != m_currentWidth || height != m_currentHeight) {
        if (m_stagesurf) {
            gs_stagesurface_destroy(m_stagesurf);
            m_stagesurf = nullptr;
        }
        m_stagesurf = gs_stagesurf_create(width, height, GS_BGRA);
        m_currentWidth = width;
        m_currentHeight = height;
    }

    // Renderizar la fuente en la textura fuera de pantalla
    struct vec4 transparent = {0.0f, 0.0f, 0.0f, 0.0f};
    if (gs_texrender_begin(m_texrender, width, height)) {
        gs_clear(GS_CLEAR_COLOR, &transparent, 0.0f, 0.0f);
        obs_source_video_render(m_source);
        gs_texrender_end(m_texrender);
    }

    gs_texture_t *texture = gs_texrender_get_texture(m_texrender);
    if (!texture || !m_stagesurf) {
        return;
    }

    // Descargar textura a la memoria de trabajo de CPU
    gs_stage_texture(m_stagesurf, texture);

    uint8_t *data = nullptr;
    uint32_t linesize = 0;
    if (gs_stagesurf_map(m_stagesurf, &data, &linesize)) {
        size_t frameSize = static_cast<size_t>(linesize) * height;

        VideoFramePacket packet;
        packet.timestamp_ns = current_time_ns;
        packet.width = width;
        packet.height = height;
        packet.is_keyframe = true;
        packet.data.resize(frameSize);
        std::memcpy(packet.data.data(), data, frameSize);

        gs_stagesurf_unmap(m_stagesurf);

        // Almacenar en el búfer circular protegiendo la sección crítica
        {
            std::lock_guard<std::mutex> lock(m_bufferMutex);
            m_ringBuffer.push_back(std::move(packet));
            m_currentMemoryUsage += frameSize;
            m_lastFrameTimeNs = current_time_ns;

            PruneOldFrames(current_time_ns);
            m_status = BufferStatus::Buffering;
        }
    }
}

void SourceBuffer::PruneOldFrames(uint64_t current_time_ns)
{
    uint64_t duration_ns = static_cast<uint64_t>(m_durationSeconds) * 1000000000ULL;
    uint64_t threshold_ns = (current_time_ns > duration_ns) ? (current_time_ns - duration_ns) : 0;

    while (!m_ringBuffer.empty() && m_ringBuffer.front().timestamp_ns < threshold_ns) {
        m_currentMemoryUsage -= m_ringBuffer.front().data.size();
        m_ringBuffer.pop_front();
    }
}

void SourceBuffer::SetBufferDuration(int seconds)
{
    std::lock_guard<std::mutex> lock(m_bufferMutex);
    m_durationSeconds = std::clamp(seconds, 1, 300);
}

void SourceBuffer::ClearBufferInternal()
{
    m_ringBuffer.clear();
    m_currentMemoryUsage = 0;
}

std::string SourceBuffer::GetStatusString() const
{
    switch (m_status) {
    case BufferStatus::Inactive:
        return "Inactivo";
    case BufferStatus::Initializing:
        return "Inicializando...";
    case BufferStatus::Buffering:
        return "Búfer Activo";
    case BufferStatus::Saving:
        return "Guardando Clip...";
    case BufferStatus::Error:
        return "Error en Fuente";
    default:
        return "Desconocido";
    }
}

size_t SourceBuffer::GetEstimatedMemoryBytes() const
{
    std::lock_guard<std::mutex> lock(m_bufferMutex);
    return m_currentMemoryUsage;
}

size_t SourceBuffer::GetBufferedFrameCount() const
{
    std::lock_guard<std::mutex> lock(m_bufferMutex);
    return m_ringBuffer.size();
}

double SourceBuffer::GetBufferedDurationSeconds() const
{
    std::lock_guard<std::mutex> lock(m_bufferMutex);
    if (m_ringBuffer.size() < 2) {
        return 0.0;
    }
    uint64_t start = m_ringBuffer.front().timestamp_ns;
    uint64_t end = m_ringBuffer.back().timestamp_ns;
    return static_cast<double>(end - start) / 1000000000.0;
}

std::vector<VideoFramePacket> SourceBuffer::ExtractReplaySlice(uint64_t start_time_ns, uint64_t end_time_ns)
{
    std::lock_guard<std::mutex> lock(m_bufferMutex);
    std::vector<VideoFramePacket> slice;

    if (m_ringBuffer.empty()) {
        return slice;
    }

    slice.reserve(m_ringBuffer.size());
    for (const auto &packet : m_ringBuffer) {
        if (packet.timestamp_ns >= start_time_ns && packet.timestamp_ns <= end_time_ns) {
            slice.push_back(packet);
        }
    }

    return slice;
}

} // namespace SVReplay
