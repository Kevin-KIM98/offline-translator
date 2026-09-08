/* Shim for RNNoise v0.2: vec.h / vec_neon.h include Opus' os_support.h, which is not
 * shipped in the rnnoise tree. Provides the three memory macros they use. */
#ifndef RNNOISE_SHIM_OS_SUPPORT_H
#define RNNOISE_SHIM_OS_SUPPORT_H

#include <stdlib.h>
#include <string.h>

#ifndef OPUS_COPY
#define OPUS_COPY(dst, src, n) (memcpy((dst), (src), (n) * sizeof(*(dst))))
#endif
#ifndef OPUS_MOVE
#define OPUS_MOVE(dst, src, n) (memmove((dst), (src), (n) * sizeof(*(dst))))
#endif
#ifndef OPUS_CLEAR
#define OPUS_CLEAR(dst, n) (memset((dst), 0, (n) * sizeof(*(dst))))
#endif

#endif
