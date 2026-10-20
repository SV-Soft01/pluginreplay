#include "camera-pipeline.hpp"

#include <obs.h>
#include <util/base.h>
#include <util/platform.h>
#include <callback/calldata.h>
#include <callback/proc.h>
#include <callback/signal.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <vector>

namespace SVReplay {

namespace fs = std::filesystem;

namespace {

std::atomic<uint32_t> g_nextPipelineUid{1};

constexpr uint64_t kRetryDelayNs = 2ULL * 1000000000ULL;
constexpr int kStopWaitMs = 3000;
constexpr uint32_t kMinDimension = 16;

uint32_t EvenDimension(uint32_t value)
{
    return value & ~1u; // NV12 exige dimensiones pares
}

bool EncoderRegistered(const char *id)
{
    return obs_encoder_get_display_name(id) != nullptr;
}

std::vector<const char *> EncoderCandidates(EncoderPreference preference)
{
    std::vector<const char *> candidates;
    switch (preference) {
    case EncoderPreference::Nvenc:
        candidates = {"obs_nvenc_h264_tex"};
        break;
    case EncoderPreference::X264:
        candidates = {"obs_x264"};
        break;
    case EncoderPreference::Auto:
    default:
        candidates = {"obs_nvenc_h264_tex", "h264_texture_amf", "obs_qsv11_v2", "obs_x264"};
        break;
    }

    std::vector<const char *> available;
    for (const char *id : candidates) {
        if (EncoderRegistered(id)) {
            available.push_back(id);
        }
    }
    return available;
}

obs_data_t *BuildEncoderSettings(const char *encoderId, const ReplaySettings &settings)
{
    obs_data_t *data = obs_encoder_defaults(encoderId);
    if (!data) {
        data = obs_data_create();
    }

    obs_data_set_string(data, "rate_control", "CBR");
    obs_data_set_int(data, "bitrate", settings.videoBitrateKbps);
    // Un fotograma clave por segundo: el clip guardado empieza en un
    // fotograma clave, así que la precisión del corte es de ~1 s.
    obs_data_set_int(data, "keyint_sec", 1);

    if (std::strcmp(encoderId, "obs_x264") == 0) {
        obs_data_set_string(data, "preset", "veryfast");
        obs_data_set_string(data, "profile", "high");
    } else if (std::strcmp(encoderId, "obs_nvenc_h264_tex") == 0) {
        obs_data_set_string(data, "preset2", "p5");
        obs_data_set_string(data, "tune", "hq");
        obs_data_set_string(data, "profile", "high");
    }

    return data;
}

} // namespace

CameraPipeline::CameraPipeline(std::string sourceName, ClipSavedCallback onSaved)
    : m_name(std::move(sourceName)),
      m_onSaved(std::move(onSaved)),
      m_uid(g_nextPipelineUid.fetch_add(1))
{
    std::lock_guard<std::mutex> lock(m_mutex);
    SetStateLocked(PipelineState::Stopped, "Pendiente de iniciar.");
}

CameraPipeline::~CameraPipeline()
{
    Retire();
}

void CameraPipeline::Retire()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_retired = true;
    StopLocked();
}

// ---------------------------------------------------------------------------
// Supervisión periódica
// ---------------------------------------------------------------------------

void CameraPipeline::Tick(const ReplaySettings &settings)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_retired) {
        return;
    }

    const uint64_t now = os_gettime_ns();

    if (m_restartRequested.exchange(false)) {
        blog(LOG_INFO, "[SV MultiCam] Reiniciando cámara '%s' por cambio de ajustes.", m_name.c_str());
        StartLocked(settings);
        return;
    }

    switch (m_state) {
    case PipelineState::Stopped:
        StartLocked(settings);
        break;

    case PipelineState::WaitingForSource:
    case PipelineState::Error:
        if (now >= m_nextRetryNs) {
            StartLocked(settings);
        }
        break;

    case PipelineState::Running: {
        if (!m_output || !obs_output_active(m_output)) {
            blog(LOG_WARNING, "[SV MultiCam] La salida de '%s' se detuvo inesperadamente. Reintentando.",
                 m_name.c_str());
            TeardownMediaLocked();
            SetStateLocked(PipelineState::Error, "La salida de repetición se detuvo. Reintentando...");
            ScheduleRetryLocked();
            break;
        }

        if (m_source && obs_source_removed(m_source)) {
            blog(LOG_WARNING, "[SV MultiCam] La fuente '%s' fue eliminada de OBS.", m_name.c_str());
            StopLocked();
            SetStateLocked(PipelineState::WaitingForSource, "La fuente fue eliminada de OBS.");
            ScheduleRetryLocked();
            break;
        }

        if (m_source) {
            const uint32_t w = EvenDimension(obs_source_get_width(m_source));
            const uint32_t h = EvenDimension(obs_source_get_height(m_source));
            if (w >= kMinDimension && h >= kMinDimension && (w != m_width || h != m_height)) {
                blog(LOG_INFO, "[SV MultiCam] '%s' cambió de resolución (%ux%u -> %ux%u). Reiniciando.",
                     m_name.c_str(), m_width, m_height, w, h);
                StartLocked(settings);
            }
        }
        break;
    }

    case PipelineState::Starting:
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Arranque y parada
// ---------------------------------------------------------------------------

void CameraPipeline::StartLocked(const ReplaySettings &settings)
{
    TeardownMediaLocked();

    if (m_retired) {
        return;
    }

    if (settings.outputDirectory.empty()) {
        SetStateLocked(PipelineState::Error, "Falta configurar la carpeta de destino.");
        ScheduleRetryLocked();
        return;
    }

    std::error_code ec;
    fs::create_directories(fs::u8path(settings.outputDirectory), ec);
    if (ec) {
        SetStateLocked(PipelineState::Error, "No se pudo crear la carpeta de destino: " + ec.message());
        ScheduleRetryLocked();
        return;
    }

    SetStateLocked(PipelineState::Starting, "Iniciando...");

    if (m_source && obs_source_removed(m_source)) {
        ReleaseSourceLocked();
    }
    if (!m_source) {
        m_source = obs_get_source_by_name(m_name.c_str());
    }
    if (!m_source) {
        SetStateLocked(PipelineState::WaitingForSource, "La fuente no existe en esta colección de escenas.");
        ScheduleRetryLocked();
        return;
    }

    uint32_t width = EvenDimension(obs_source_get_width(m_source));
    uint32_t height = EvenDimension(obs_source_get_height(m_source));
    if (width < kMinDimension || height < kMinDimension) {
        // Algunos dispositivos solo entregan video cuando están "visibles":
        // los despertamos mientras esperamos su resolución.
        if (!m_wakeRefs) {
            obs_source_inc_showing(m_source);
            obs_source_inc_active(m_source);
            m_wakeRefs = true;
        }
        SetStateLocked(PipelineState::WaitingForSource, "Esperando video de la fuente...");
        ScheduleRetryLocked();
        return;
    }

    struct obs_video_info ovi;
    if (!obs_get_video_info(&ovi)) {
        SetStateLocked(PipelineState::Error, "OBS aún no tiene el video inicializado.");
        ScheduleRetryLocked();
        return;
    }
    // El video de esta cámara usa la resolución nativa de la fuente.
    ovi.base_width = ovi.output_width = width;
    ovi.base_height = ovi.output_height = height;

    m_view = obs_view_create();
    if (!m_view) {
        SetStateLocked(PipelineState::Error, "No se pudo crear la vista de video.");
        ScheduleRetryLocked();
        return;
    }
    obs_view_set_source(m_view, 0, m_source);
    m_video = obs_view_add2(m_view, &ovi);

    // La vista ya mantiene la fuente activa; liberamos nuestras referencias de "despertar".
    if (m_wakeRefs) {
        obs_source_dec_showing(m_source);
        obs_source_dec_active(m_source);
        m_wakeRefs = false;
    }

    if (!m_video) {
        TeardownMediaLocked();
        SetStateLocked(PipelineState::Error, "OBS no pudo crear la salida de video de la cámara.");
        ScheduleRetryLocked();
        return;
    }

    m_width = width;
    m_height = height;

    const std::vector<const char *> candidates = EncoderCandidates(settings.encoder);
    if (candidates.empty()) {
        TeardownMediaLocked();
        SetStateLocked(PipelineState::Error, "El codificador seleccionado no está disponible en este OBS.");
        ScheduleRetryLocked();
        return;
    }

    std::string lastError;
    for (const char *encoderId : candidates) {
        std::string error;
        if (TryStartWithEncoderLocked(encoderId, settings, error)) {
            m_encoderId = encoderId;
            m_bufferSeconds = settings.bufferSeconds;
            m_bitrateKbps = settings.videoBitrateKbps;
            m_startedNs = os_gettime_ns();
            SetStateLocked(PipelineState::Running, "Búfer activo.");
            blog(LOG_INFO, "[SV MultiCam] Cámara '%s' activa: %ux%u, %s, %d kbps, búfer de %d s.", m_name.c_str(),
                 m_width, m_height, encoderId, m_bitrateKbps, m_bufferSeconds);
            return;
        }
        blog(LOG_WARNING, "[SV MultiCam] '%s': el codificador %s falló: %s", m_name.c_str(), encoderId,
             error.c_str());
        lastError = error;
    }

    TeardownMediaLocked();
    SetStateLocked(PipelineState::Error, "No se pudo iniciar ningún codificador. " + lastError);
    ScheduleRetryLocked();
}

bool CameraPipeline::TryStartWithEncoderLocked(const char *encoderId, const ReplaySettings &settings,
                                               std::string &error)
{
    const std::string tag = "svmc_" + std::to_string(m_uid);

    // --- Codificador de video -------------------------------------------
    obs_data_t *videoSettings = BuildEncoderSettings(encoderId, settings);
    const std::string videoName = tag + "_video";
    m_videoEncoder = obs_video_encoder_create(encoderId, videoName.c_str(), videoSettings, nullptr);
    obs_data_release(videoSettings);
    if (!m_videoEncoder) {
        error = "No se pudo crear el codificador de video.";
        return false;
    }
    obs_encoder_set_video(m_videoEncoder, m_video);

    // --- Codificador de audio (pista elegida de la mezcla de OBS) ---------
    obs_data_t *audioSettings = obs_data_create();
    obs_data_set_int(audioSettings, "bitrate", 160);
    const std::string audioName = tag + "_audio";
    m_audioEncoder = obs_audio_encoder_create("ffmpeg_aac", audioName.c_str(), audioSettings,
                                              static_cast<size_t>(settings.audioTrack - 1), nullptr);
    obs_data_release(audioSettings);
    if (!m_audioEncoder) {
        error = "No se pudo crear el codificador de audio AAC.";
        TeardownEncodeChainLocked();
        return false;
    }
    obs_encoder_set_audio(m_audioEncoder, obs_get_audio());

    // --- Salida "replay_buffer" de OBS -----------------------------------
    // El nombre temporal es único por cámara; el gestor lo renombra después
    // al formato definitivo (prefijo_jugada_fecha_cámara.mp4).
    const std::string fileFormat = tag + "_%CCYY%MM%DD_%hh%mm%ss";
    obs_data_t *outputSettings = obs_data_create();
    obs_data_set_string(outputSettings, "directory", settings.outputDirectory.c_str());
    obs_data_set_string(outputSettings, "format", fileFormat.c_str());
    obs_data_set_string(outputSettings, "extension", "mp4");
    obs_data_set_int(outputSettings, "max_time_sec", settings.bufferSeconds);
    obs_data_set_int(outputSettings, "max_size_mb", 0);
    const std::string outputName = tag + "_replay";
    m_output = obs_output_create("replay_buffer", outputName.c_str(), outputSettings, nullptr);
    obs_data_release(outputSettings);
    if (!m_output) {
        error = "OBS no pudo crear la salida de búfer de repetición.";
        TeardownEncodeChainLocked();
        return false;
    }

    obs_output_set_video_encoder(m_output, m_videoEncoder);
    obs_output_set_audio_encoder(m_output, m_audioEncoder, 0);

    m_savedContext = new SavedContext{this, m_output};
    signal_handler_t *signalHandler = obs_output_get_signal_handler(m_output);
    signal_handler_connect(signalHandler, "saved", &CameraPipeline::OnSavedSignal, m_savedContext);

    if (!obs_output_start(m_output)) {
        const char *lastError = obs_output_get_last_error(m_output);
        error = (lastError && *lastError) ? lastError : "obs_output_start devolvió error.";
        TeardownEncodeChainLocked();
        return false;
    }

    return true;
}

void CameraPipeline::StopLocked()
{
    TeardownMediaLocked();
    ReleaseSourceLocked();
    m_width = 0;
    m_height = 0;
    SetStateLocked(PipelineState::Stopped, "Detenido.");
}

void CameraPipeline::TeardownEncodeChainLocked()
{
    if (m_output) {
        signal_handler_t *signalHandler = obs_output_get_signal_handler(m_output);
        if (signalHandler) {
            signal_handler_disconnect(signalHandler, "saved", &CameraPipeline::OnSavedSignal, m_savedContext);
        }

        if (obs_output_active(m_output)) {
            obs_output_stop(m_output);
            int waited = 0;
            while (obs_output_active(m_output) && waited < kStopWaitMs) {
                os_sleep_ms(50);
                waited += 50;
            }
            if (obs_output_active(m_output)) {
                obs_output_force_stop(m_output);
            }
        }
        obs_output_release(m_output);
        m_output = nullptr;
    }

    delete m_savedContext;
    m_savedContext = nullptr;

    if (m_audioEncoder) {
        obs_encoder_release(m_audioEncoder);
        m_audioEncoder = nullptr;
    }
    if (m_videoEncoder) {
        obs_encoder_release(m_videoEncoder);
        m_videoEncoder = nullptr;
    }
}

void CameraPipeline::TeardownMediaLocked()
{
    TeardownEncodeChainLocked();

    if (m_view) {
        obs_view_remove(m_view);
        obs_view_set_source(m_view, 0, nullptr);
        obs_view_destroy(m_view);
        m_view = nullptr;
    }
    m_video = nullptr;
}

void CameraPipeline::ReleaseSourceLocked()
{
    if (m_source) {
        if (m_wakeRefs) {
            obs_source_dec_showing(m_source);
            obs_source_dec_active(m_source);
            m_wakeRefs = false;
        }
        obs_source_release(m_source);
        m_source = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Guardado
// ---------------------------------------------------------------------------

bool CameraPipeline::RequestSave()
{
    // Si la cámara se está reiniciando en este instante no tiene búfer útil.
    std::unique_lock<std::mutex> lock(m_mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        return false;
    }
    if (m_state != PipelineState::Running || !m_output || !obs_output_active(m_output)) {
        return false;
    }

    proc_handler_t *procHandler = obs_output_get_proc_handler(m_output);
    if (!procHandler) {
        return false;
    }

    calldata_t cd = {};
    const bool ok = proc_handler_call(procHandler, "save", &cd);
    calldata_free(&cd);
    return ok;
}

void CameraPipeline::OnSavedSignal(void *param, calldata_t * /*cd*/)
{
    auto *context = static_cast<SavedContext *>(param);
    if (!context || !context->owner || !context->output) {
        return;
    }

    std::string path;
    calldata_t cd = {};
    proc_handler_t *procHandler = obs_output_get_proc_handler(context->output);
    if (procHandler && proc_handler_call(procHandler, "get_last_replay", &cd)) {
        const char *lastPath = nullptr;
        if (calldata_get_string(&cd, "path", &lastPath) && lastPath) {
            path = lastPath;
        }
    }
    calldata_free(&cd);

    if (context->owner->m_onSaved) {
        context->owner->m_onSaved(context->owner->m_name, path);
    }
}

// ---------------------------------------------------------------------------
// Estado
// ---------------------------------------------------------------------------

void CameraPipeline::SetStateLocked(PipelineState state, const std::string &detail)
{
    m_state = state;
    m_detail = detail;
    PublishSnapshotLocked();
}

void CameraPipeline::ScheduleRetryLocked()
{
    m_nextRetryNs = os_gettime_ns() + kRetryDelayNs;
}

void CameraPipeline::PublishSnapshotLocked()
{
    std::lock_guard<std::mutex> lock(m_snapshotMutex);
    m_snapshot.name = m_name;
    m_snapshot.state = m_state;
    m_snapshot.detail = m_detail;
    m_snapshot.width = m_width;
    m_snapshot.height = m_height;
    m_snapshot.bitrateKbps = m_bitrateKbps;
    m_snapshot.bufferTargetSeconds = m_bufferSeconds;
    m_snapshot.startedNs = m_startedNs;

    const char *displayName = m_encoderId.empty() ? nullptr : obs_encoder_get_display_name(m_encoderId.c_str());
    m_snapshot.encoderName = displayName ? displayName : "";
}

CameraStatus CameraPipeline::GetStatus() const
{
    CameraStatus status;
    {
        std::lock_guard<std::mutex> lock(m_snapshotMutex);
        status = m_snapshot;
    }

    if (status.state == PipelineState::Running && status.startedNs > 0) {
        const uint64_t now = os_gettime_ns();
        const double elapsed = (now > status.startedNs) ? static_cast<double>(now - status.startedNs) / 1e9 : 0.0;
        status.bufferedSeconds = std::min(elapsed, static_cast<double>(status.bufferTargetSeconds));
    }
    return status;
}

} // namespace SVReplay