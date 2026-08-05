#pragma once

// PlatformIO normally supplies these through build_flags/extra_scripts. Keep
// fallbacks here so editor indexers and simulator-like tools still parse files.
// The version arrives through a generated header rather than a -D, so that moving
// HEAD does not invalidate every ESP-IDF object file. Envs that still pass it on
// the command line keep working: the define below only fills a gap.
#if defined(__has_include)
#if __has_include("AppVersionGenerated.h")
#include "AppVersionGenerated.h"
#endif
#endif

#ifndef CROSSINK_VERSION
#ifdef CROSSINK_VERSION_GENERATED
#define CROSSINK_VERSION CROSSINK_VERSION_GENERATED
#else
#define CROSSINK_VERSION "dev"
#endif
#endif

#ifndef CROSSINK_BUILD_ENV
#define CROSSINK_BUILD_ENV "unknown"
#endif

#ifndef CROSSINK_FIRMWARE_DEVICE_TYPE
#define CROSSINK_FIRMWARE_DEVICE_TYPE "unknown"
#endif
