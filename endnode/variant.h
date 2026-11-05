#pragma once

// One variant id for the OTA check. The cellular env does not define FEATURE_WIFI.
#ifdef FEATURE_WIFI
#define VARIANT_ID "wifi"
#else
#define VARIANT_ID "cell"
#endif

#ifndef API_STAGE
#define API_STAGE "prod"
#endif
