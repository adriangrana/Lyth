#ifndef ASSERT_H
#define ASSERT_H

#include "panic.h"

#ifndef NDEBUG
#define KASSERT(expr) \
    do { \
        if (!(expr)) { \
            panic_assert_fail(#expr, __FILE__, __LINE__, __func__); \
        } \
    } while (0)

#define KASSERT_MSG(expr, msg) \
    do { \
        if (!(expr)) { \
            panic_assert_fail_msg(#expr, (msg), __FILE__, __LINE__, __func__); \
        } \
    } while (0)
#else
#define KASSERT(expr) ((void)0)
#define KASSERT_MSG(expr, msg) ((void)0)
#endif

#endif
