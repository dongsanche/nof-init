#pragma once

#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>

#define LOG(fmt, ...) do { \
    std::fprintf(stderr, "[LOG] " fmt "\n", ##__VA_ARGS__); \
} while (0)

#define ERR(fmt, ...) do { \
    std::fprintf(stderr, "[ERR] %s:%d " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); \
} while (0)

#define ERRNO(fmt, ...) do { \
    std::fprintf(stderr, "[ERR] %s:%d " fmt ": %s\n", __FILE__, __LINE__, \
                 ##__VA_ARGS__, std::strerror(errno)); \
} while (0)
