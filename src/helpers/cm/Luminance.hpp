#pragma once

namespace NColorManagement {
    // Wayland and CTA-861.3 encode minimum luminance in units of 0.0001 cd/m².
    double encodeMinimumLuminance(double nits);
}
