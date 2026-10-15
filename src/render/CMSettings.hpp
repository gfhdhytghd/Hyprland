#pragma once

#include "types.hpp"
#include <vector>

namespace Render {
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
