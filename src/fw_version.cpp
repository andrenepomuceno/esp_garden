#include "core/fw_version.h"
#include <stdlib.h>

void
fwVersionParse(const char* version, int out[3])
{
    out[0] = out[1] = out[2] = 0;

    if (version == nullptr) {
        return;
    }

    const char* cursor = version;
    for (int i = 0; i < 3; ++i) {
        // Skip a leading 'v' on the first component: ThingsBoard lets an
        // operator type the version by hand and "v2.1.0" is a common spelling.
        if (i == 0 && (*cursor == 'v' || *cursor == 'V')) {
            ++cursor;
        }

        char* end = nullptr;
        const long value = strtol(cursor, &end, 10);
        if (end == cursor) {
            return; // nothing numeric here; the rest stays 0
        }

        out[i] = (value < 0) ? 0 : (int)value;

        if (*end != '.') {
            return; // trailing suffix such as "-rc1" ends the version
        }
        cursor = end + 1;
    }
}

int
fwVersionCompare(const char* a, const char* b)
{
    int va[3];
    int vb[3];
    fwVersionParse(a, va);
    fwVersionParse(b, vb);

    for (int i = 0; i < 3; ++i) {
        if (va[i] < vb[i]) {
            return -1;
        }
        if (va[i] > vb[i]) {
            return 1;
        }
    }

    return 0;
}

bool
fwVersionDiffers(const char* offered, const char* current)
{
    return fwVersionCompare(offered, current) != 0;
}
