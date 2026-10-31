#include "Core.h"

#if defined(__SSE__) || defined(_M_X64) || defined(_M_IX86)
 #include <xmmintrin.h>
 #define TINCTURE_SSE 1
#endif

namespace tincture
{
ScopedNoDenormals::ScopedNoDenormals() noexcept
{
   #if TINCTURE_SSE
    saved = (intptr_t) _mm_getcsr();
    _mm_setcsr ((unsigned) saved | 0x8040);   // FTZ | DAZ
   #elif defined(__aarch64__)
    uint64_t fpcr;
    asm volatile ("mrs %0, fpcr" : "=r"(fpcr));
    saved = (intptr_t) fpcr;
    asm volatile ("msr fpcr, %0" : : "r"(fpcr | (1ull << 24)));   // FZ
   #endif
}

ScopedNoDenormals::~ScopedNoDenormals() noexcept
{
   #if TINCTURE_SSE
    _mm_setcsr ((unsigned) saved);
   #elif defined(__aarch64__)
    asm volatile ("msr fpcr, %0" : : "r"((uint64_t) saved));
   #endif
}
} // namespace tincture
