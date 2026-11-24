#pragma once

#include "types.hpp"
#include <cstdint>
#include <vector>

#include "../helpers/TransferFunction.hpp"

struct SSurfaceState;

namespace Render {
    struct STextureColorSource {
        NColorManagement::PImageDescription description;
        bool                                useSurfacePolicy = false;
    };

    STextureColorSource textureColorSource(NColorManagement::PImageDescription texture, NColorManagement::PImageDescription surface,
                                           NColorManagement::PImageDescription workFallback);

    // The transfer function a source is decoded with. render:cm_sdr_eotf overrides the described sRGB / gamma 2.2
    // of surface content: always for surfaces without a CM extension, and only gamma22force for CM-aware ones.
    NColorManagement::eTransferFunction resolveSourceTF(NColorManagement::eTransferFunction describedTF, bool surfaceSource, bool cmAwareSurface, NTransferFunction::eTF sdrEOTF);

    // The same policy for a surface: it counts as CM-aware while a description is applied to it.
    NColorManagement::eTransferFunction surfaceSourceTF(const SSurfaceState& current, NColorManagement::eTransferFunction source, NTransferFunction::eTF policy);

    // Resolved policy inputs, rather than the identity/address of a surface or
    // monitor. Equal inputs must produce equal settings across nested draws.
    struct SCMSettingsInputs {
        NColorManagement::eTransferFunction sourceTF                                   = NColorManagement::CM_TRANSFER_FUNCTION_GAMMA22;
        int                                 tonemapMode                                = 1;
        bool                                needsSDRmod                                = false;
        float                               sdrBrightness                              = 1.F;
        float                               sdrSaturation                              = 1.F;
        float                               sdrMinLuminance                            = -1.F;
        bool                                operator==(const SCMSettingsInputs&) const = default;
    };

    class CCMSettingsCache {
      public:
        SCMSettings get(NColorManagement::PImageDescription source, NColorManagement::PImageDescription target, const SCMSettingsInputs& inputs);
        void        clear();

      private:
        friend class CCMSettingsCacheTestAccessor;

        struct SEntry {
            uint64_t          sourceId = 0;
            uint64_t          targetId = 0;
            SCMSettingsInputs inputs;
            SCMSettings       settings;
        };
        std::vector<SEntry> m_entries;
    };
}
