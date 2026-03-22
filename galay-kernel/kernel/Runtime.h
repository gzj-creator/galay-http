#ifndef GALAY_HTTP_KERNEL_RUNTIME_COMPAT_H
#define GALAY_HTTP_KERNEL_RUNTIME_COMPAT_H

#if defined(__has_include_next)
#if __has_include_next("galay-kernel/kernel/Runtime.h")
#include_next "galay-kernel/kernel/Runtime.h"
#else
#include "/usr/local/include/galay-kernel/kernel/Runtime.h"
#endif
#else
#include "/usr/local/include/galay-kernel/kernel/Runtime.h"
#endif

#include "galay-kernel/kernel/Coroutine.h"

#endif // GALAY_HTTP_KERNEL_RUNTIME_COMPAT_H
