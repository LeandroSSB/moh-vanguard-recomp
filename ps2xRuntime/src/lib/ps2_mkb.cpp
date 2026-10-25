#include "runtime/ps2_mkb.h"

// Vanguard M5-T19: RED stub - the layer is not implemented yet. The
// signatures are final; every behavior test below fails against this
// state until the implementation lands.

namespace ps2x
{
    bool MkbLoadConfig(const char *path, MkbConfig &out)
    {
        (void)path;
        (void)out;
        return false;
    }

    void MkbApplyMouseDelta(float &rx, float &ry, float dx, float dy,
                            const MkbConfig &cfg)
    {
        (void)dx; (void)dy; (void)cfg;
        rx = 0.0f;
        ry = 0.0f;
    }

    void MkbStickToBytes(float rx, float ry, const MkbConfig &cfg,
                         uint8_t &outRx, uint8_t &outRy)
    {
        (void)rx; (void)ry; (void)cfg;
        outRx = 128u;
        outRy = 128u;
    }

    uint16_t MkbMouseButtonsToPad(uint32_t mouseButtons)
    {
        (void)mouseButtons;
        return 0u;
    }
}
