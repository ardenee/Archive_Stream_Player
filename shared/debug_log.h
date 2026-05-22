#pragma once

#include <stdarg.h>

void DebugLogConfigure(bool enabled, const char* logPath, bool resetFile);
void DebugLogShutdown();

void DebugLogWritePlain(const char* level, const char* area, const char* eventName, const char* fmt, ...);
void DebugLogWriteVPlain(const char* level, const char* area, const char* eventName, const char* fmt, va_list args);

void DebugLogWriteRaw(const char* level, const char* area, const char* eventName, const char* fmt, ...);
void DebugLogWriteVRaw(const char* level, const char* area, const char* eventName, const char* fmt, va_list args);

void DebugLogWriteEx(const char* sourceFile, int sourceLine, const char* level, const char* area, const char* eventName, const char* fmt, ...);
void DebugLogWriteVEx(const char* sourceFile, int sourceLine, const char* level, const char* area, const char* eventName, const char* fmt, va_list args);

bool DebugLogIsEnabled();
const char* DebugLogGetPath();

#ifndef DEBUG_LOG_IMPLEMENTATION
#define DebugLogWrite(level, area, eventName, fmt, ...) DebugLogWriteEx(__FILE__, __LINE__, level, area, eventName, fmt, ##__VA_ARGS__)
#define DebugLogWriteV(level, area, eventName, fmt, args) DebugLogWriteVEx(__FILE__, __LINE__, level, area, eventName, fmt, args)
#endif