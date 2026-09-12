#pragma once

// lib/GfxRenderer/Bitmap.cpp includes <Logging.h>, whose real implementation
// pulls in Arduino.h. Host tests only need the macros to disappear.
#define LOG_DBG(...) ((void)0)
#define LOG_INF(...) ((void)0)
#define LOG_ERR(...) ((void)0)
