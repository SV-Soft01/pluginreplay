#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace SVReplay {

// Codificador de video que usará cada cámara.
enum class EncoderPreference {
    Auto = 0,  // NVENC -> AMD -> QuickSync -> x264 (el primero que funcione)
    Nvenc = 1, // Solo NVIDIA NVENC H.264
    X264 = 2   // Solo x264 (CPU)
};

// Ajustes globales del plugin (se guardan en config.json).
struct ReplaySettings {
    int bufferSeconds = 10;
    std::string outputDirectory;
    std::string filePrefix = "replay";
    int videoBitrateKbps = 12000;
    EncoderPreference encoder = EncoderPreference::Auto;
    int audioTrack = 6; // Pista 1..6 de la mezcla de audio de OBS

    void Clamp()
    {
        bufferSeconds = std::clamp(bufferSeconds, 1, 120);
        videoBitrateKbps = std::clamp(videoBitrateKbps, 1000, 100000);
        audioTrack = std::clamp(audioTrack, 1, 6);
        if (filePrefix.empty()) {
            filePrefix = "replay";
        }
    }

    // Ajustes que quedan "incrustados" en el pipeline de codificación:
    // si cambian hay que reiniciar las cámaras. El prefijo se aplica al
    // renombrar el archivo, así que no necesita reinicio.
    bool RequiresPipelineRestart(const ReplaySettings &other) const
    {
        return bufferSeconds != other.bufferSeconds || outputDirectory != other.outputDirectory ||
               videoBitrateKbps != other.videoBitrateKbps || encoder != other.encoder ||
               audioTrack != other.audioTrack;
    }
};

enum class PipelineState {
    Stopped,          // Aún no iniciado
    WaitingForSource, // La fuente no existe o todavía no entrega video
    Starting,
    Running,
    Error
};

// Instantánea del estado de una cámara (para el monitor de la interfaz).
struct CameraStatus {
    std::string name;
    PipelineState state = PipelineState::Stopped;
    std::string detail;
    uint32_t width = 0;
    uint32_t height = 0;
    std::string encoderName;
    int bitrateKbps = 0;
    int bufferTargetSeconds = 0;
    double bufferedSeconds = 0.0;
    uint64_t startedNs = 0; // interno: instante en que empezó a grabar el búfer
};

// Resultado de una orden "guardar repetición" (todas las cámaras).
struct SaveResult {
    bool success = false;
    uint32_t playId = 0;
    int totalClips = 0;
    std::vector<std::string> savedFiles;
    std::vector<std::string> errors;
};

} // namespace SVReplay