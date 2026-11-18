/* 本文件由 tools/gen_voice.py 生成，请勿手改。 */

#include "voice_data.h"

extern const uint8_t _binary_voice_data_bin_start[];

const voice_clip_t g_voice_clips[VOICE_MAX] = {
    [0] = { .ulaw = _binary_voice_data_bin_start + 0, .samples = 18276 },
    [1] = { .ulaw = _binary_voice_data_bin_start + 18276, .samples = 36462 },
    [2] = { .ulaw = _binary_voice_data_bin_start + 54738, .samples = 38591 },
    [3] = { .ulaw = _binary_voice_data_bin_start + 93329, .samples = 34884 },
};
