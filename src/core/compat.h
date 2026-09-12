#pragma once

#include <cstdint>

#include "version_detect.h"

namespace trinity::core
{
    // The single game build this menu has been validated against. Every
    // surface that names a version - the installer, the overlay warning, the
    // menu header, the README - reads these, so a retarget touches one place.
    inline constexpr uint16_t    kSupportedRevision = 2850;
    inline constexpr const char* kSupportedTitle    = "Crimson Desert 2.02.00";
    inline constexpr const char* kSupportedPE       = "1.0.0.2850";

    inline bool GameVersionSupported()
    {
        return GetGameVersion().revision == kSupportedRevision;
    }
}
