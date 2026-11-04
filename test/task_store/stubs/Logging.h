#pragma once

#include <cstdio>

// Les erreurs journalisees sortent sur stdout : un test qui echoue dit pourquoi.
#define LOG_ERR(tag, ...) (std::printf("ERR %s: ", tag), std::printf(__VA_ARGS__), std::printf("\n"))
#define LOG_INF(...)
#define LOG_DBG(...)
