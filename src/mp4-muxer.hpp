#pragma once

#include "source-buffer.hpp"
#include <string>
#include <vector>
#include <cstdint>

namespace SVReplay {

struct MuxerOptions {
    std::string outputDirectory;
    std::string filenamePrefix = "replay";
    uint32_t playId = 1;
    std::string timestampStr;
    std::string sourceName;
    uint32_t fps = 60;
};

class MP4Muxer {
public:
    MP4Muxer();
    ~MP4Muxer();

    // Genera el archivo MP4 sincronizado a partir de los paquetes extraídos
    static bool SaveClipsToMP4(
        const MuxerOptions &options,
        const std::vector<VideoFramePacket> &frames,
        std::string &outFinalPath,
        std::string &outErrorMessage
    );

    // Genera la ruta y nombre de archivo formateado
    static std::string BuildFilename(const MuxerOptions &options);

private:
    // Limpieza de caracteres no válidos para nombres de archivo en Windows
    static std::string SanitizeFilename(const std::string &name);
};

} // namespace SVReplay
