#ifdef DESKBEAM_WINDOWS
#define NOMINMAX
#include <windows.h>
#include <wingdi.h>
#include <cstdio>

// DISPLAYCONFIG_SDR_WHITE_LEVEL and DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL
// are defined in wingdi.h on Windows SDK 10.0.26100+

int main() {
    // Get display config
    UINT32 num_paths = 0, num_modes = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &num_paths, &num_modes) != ERROR_SUCCESS) {
        printf("GetDisplayConfigBufferSizes failed\n");
        return 1;
    }

    auto paths = new DISPLAYCONFIG_PATH_INFO[num_paths];
    auto modes = new DISPLAYCONFIG_MODE_INFO[num_modes];

    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &num_paths, paths, &num_modes, modes, nullptr) != ERROR_SUCCESS) {
        printf("QueryDisplayConfig failed\n");
        return 1;
    }

    printf("Found %u active display paths:\n\n", num_paths);

    for (UINT32 i = 0; i < num_paths; ++i) {
        printf("--- Display %u ---\n", i);

        // Get target name
        DISPLAYCONFIG_TARGET_DEVICE_NAME target_name = {};
        target_name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        target_name.header.size = sizeof(target_name);
        target_name.header.adapterId = paths[i].targetInfo.adapterId;
        target_name.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&target_name.header) == ERROR_SUCCESS) {
            printf("  Monitor: %ls\n", target_name.monitorFriendlyDeviceName);
        }

        // Get SDR white level
        DISPLAYCONFIG_SDR_WHITE_LEVEL sdr = {};
        sdr.header.type = (DISPLAYCONFIG_DEVICE_INFO_TYPE)DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        sdr.header.size = sizeof(sdr);
        sdr.header.adapterId = paths[i].targetInfo.adapterId;
        sdr.header.id = paths[i].targetInfo.id;

        if (DisplayConfigGetDeviceInfo(&sdr.header) == ERROR_SUCCESS) {
            float nits = 80.0f * sdr.SDRWhiteLevel / 1000.0f;
            float boost = sdr.SDRWhiteLevel / 1000.0f;
            printf("  SDR White Level: %lu (raw)\n", sdr.SDRWhiteLevel);
            printf("  SDR White Level: %.1f nits (80 * %lu / 1000)\n", nits, sdr.SDRWhiteLevel);
            printf("  Brightness boost factor: %.3f\n", boost);
            printf("  To correct: divide pixel values by %.3f\n", boost);
        } else {
            printf("  SDR White Level: not available (HDR off?)\n");
        }

        // Get advanced color info
        DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO color_info = {};
        color_info.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
        color_info.header.size = sizeof(color_info);
        color_info.header.adapterId = paths[i].targetInfo.adapterId;
        color_info.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&color_info.header) == ERROR_SUCCESS) {
            printf("  Advanced Color Supported: %s\n", color_info.advancedColorSupported ? "yes" : "no");
            printf("  Advanced Color Enabled: %s\n", color_info.advancedColorEnabled ? "yes" : "no");
            printf("  Wide Color Enforced: %s\n", color_info.wideColorEnforced ? "yes" : "no");
            printf("  Advanced Color Force Disabled: %s\n", color_info.advancedColorForceDisabled ? "yes" : "no");
            printf("  Bits Per Color Channel: %u\n", color_info.bitsPerColorChannel);
            printf("  Color Encoding: %u\n", color_info.colorEncoding);
        }

        printf("\n");
    }

    delete[] paths;
    delete[] modes;
    return 0;
}

#else
int main() { return 0; }
#endif
