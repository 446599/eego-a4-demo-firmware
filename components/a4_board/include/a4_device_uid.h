#pragma once

#include <stdint.h>
#include <stdbool.h>

/* Match the Arduino ESP.getEfuseMac()/getEspChipId() convention: the UID is
 * the device-specific final three octets, not the shared vendor OUI. */
static inline uint32_t a4_device_uid_from_efuse_mac(const uint8_t mac[6])
{
    return ((uint32_t)mac[3] << 16U) |
           ((uint32_t)mac[4] << 8U) |
           (uint32_t)mac[5];
}

/* Hardware variants use the UID shown in Device information. */
static inline bool a4_device_uid_has_frontlight(uint32_t uid)
{
    switch (uid) {
    case 12256280U:
    case 12256268U:
    case 12256208U:
    case 12256264U:
        return false;
    default:
        return true;
    }
}
