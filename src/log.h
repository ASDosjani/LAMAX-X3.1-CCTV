#ifndef LOG_H
#define LOG_H
#include <stdio.h>
#include <time.h>

/* Simple stderr logger; autorun.sh redirects stderr to the SD card log. */
#define LOG_(lvl, fmt, ...) do { \
        struct timespec ts_; clock_gettime(CLOCK_MONOTONIC, &ts_); \
        fprintf(stderr, "[%6ld.%03ld] %s " fmt "\n", (long)ts_.tv_sec, ts_.tv_nsec / 1000000L, lvl, ##__VA_ARGS__); \
    } while (0)
#define LOGI(fmt, ...) LOG_("I", fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) LOG_("W", fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) LOG_("E", fmt, ##__VA_ARGS__)

#endif
