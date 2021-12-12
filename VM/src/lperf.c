// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// This code is based on Lua 5.x implementation licensed under MIT License; see lua_LICENSE.txt for details
#include "lua.h"

#ifdef _WIN32
#include <Windows.h>
#endif

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_time.h>
#endif

#include <time.h>

static double clock_period()
{
#if defined(_WIN32)
    LARGE_INTEGER result = {0};
    QueryPerformanceFrequency(&result);
    return 1.0 / (double)(result.QuadPart);
#elif defined(__APPLE__)
    mach_timebase_info_data_t result = {0};
    mach_timebase_info(&result);
    return (double)(result.numer) / (double)(result.denom) * 1e-9;
#elif defined(__linux__)
    return 1e-9;
#else
    return 1.0 / (double)(CLOCKS_PER_SEC);
#endif
}

static double clock_timestamp()
{
#if defined(_WIN32)
    LARGE_INTEGER result = {0};
    QueryPerformanceCounter(&result);
    return (double)(result.QuadPart);
#elif defined(__APPLE__)
    return (double)(mach_absolute_time());
#elif defined(__linux__)
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec * 1e9 + now.tv_nsec;
#else
    return (double)(clock());
#endif
}

static double period;

extern void lua_setupclock(void);

void lua_setupclock(void)
{
    period = clock_period();
}

double lua_clock()
{
    return clock_timestamp() * period;
}
