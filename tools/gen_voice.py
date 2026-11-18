# -*- coding: utf-8 -*-
"""
把中文提示语合成成固件可用的 8 kHz μ-law 二进制资源。

为什么要走这一步：
  设备断网时没法联网，语音提示必须**内置在固件里**。
  esp_audio_simple_player 走 URI 拉流，本地文件协议支持不确定；
  合成后统一成 8kHz/8bit μ-law，嵌入 firmware 二进制段，
  I2S 播放时逐块解码为 16bit，零解码库、零文件系统依赖。

用法：
  python tools/gen_voice.py

输出：
  main/voice_data.bin    （嵌入固件的 μ-law 数据）
  main/voice_data.c      （每段语音的偏移和长度）
  tools/voice_preview/   （合成好的 wav，方便先试听）

改了文案就重跑一次，然后 idf.py build。
"""

import os
import struct
import sys
import wave

# 采样率固定 8k：电话级人声足够，且只占原始 16k PCM 的四分之一。
RATE = 8000

# (C 变量名, 中文文案)
CLIPS = [
    ("voice_net_ok", "网络已连接"),
    ("voice_wifi_fail", "网络连接失败，请检查路由器"),
    ("voice_need_prov", "请打开手机应用，重新配置网络"),
    ("voice_no_net", "设备未联网，请先配置网络"),
]

HERE = os.path.dirname(os.path.abspath(__file__))
FW_ROOT = os.path.dirname(HERE)
OUT_C = os.path.join(FW_ROOT, "main", "voice_data.c")
OUT_BIN = os.path.join(FW_ROOT, "main", "voice_data.bin")
PREVIEW_DIR = os.path.join(HERE, "voice_preview")


def synth(text, path):
    """SAPI 合成到 wav。优先让引擎直接吐 16k/16bit/单声道。"""
    try:
        import win32com.client
    except ImportError:
        if os.path.exists(path):
            return
        raise SystemExit("Linux 环境需先提供已有 wav：%s" % path)

    voice = win32com.client.Dispatch("SAPI.SpVoice")
    # 优先中文音色
    for v in voice.GetVoices():
        if "Chinese" in v.GetDescription() or "Huihui" in v.GetDescription():
            voice.Voice = v
            break

    stream = win32com.client.Dispatch("SAPI.SpFileStream")
    fmt = win32com.client.Dispatch("SAPI.SpAudioFormat")
    fmt.Type = 18  # SAFT16kHz16BitMono
    stream.Format = fmt
    stream.Open(path, 3)  # SSFMCreateForWrite
    voice.AudioOutputStream = stream
    voice.Rate = -1  # 稍慢一点，硬件喇叭小、语速快听不清
    voice.Speak(text)
    stream.Close()


def read_pcm(path):
    """读 wav，返回 (pcm_bytes, rate, channels, bits)。"""
    with wave.open(path, "rb") as w:
        ch, width, rate, frames = (w.getnchannels(), w.getsampwidth(),
                                   w.getframerate(), w.getnframes())
        data = w.readframes(frames)
    return data, rate, ch, width * 8


def linear2ulaw(sample):
    """Convert signed 16-bit PCM to G.711 μ-law."""
    sample = max(-32768, min(32767, sample))
    sign = 0x80 if sample < 0 else 0
    if sample < 0:
        sample = -sample
    sample += 132
    if sample > 32767:
        sample = 32767

    segment = 0
    for seg in (1, 2, 3, 4, 5, 6, 7):
        if sample >= (256 << (seg - 1)):
            segment = seg
    mantissa = (sample >> (segment + 3)) & 0x0f
    return (~(sign | (segment << 4) | mantissa)) & 0xff


def to_mono8k_ulaw(data, rate, ch, bits):
    """统一成 8 kHz / 8bit μ-law。"""
    if bits != 16:
        raise SystemExit("只支持 16bit 输出，实际 %d" % bits)

    samples = list(struct.unpack("<%dh" % (len(data) // 2), data))
    if ch == 2:
        samples = [(samples[i] + samples[i + 1]) // 2
                   for i in range(0, len(samples) - 1, 2)]

    if rate != 8000:
        ratio = 8000.0 / float(rate)
        n_out = int(len(samples) * ratio)
        out = []
        for i in range(n_out):
            pos = i / ratio
            i0 = int(pos)
            i1 = min(i0 + 1, len(samples) - 1)
            frac = pos - i0
            out.append(int(samples[i0] * (1 - frac) + samples[i1] * frac))
        samples = out

    # 去直流 + 归一化到 -12dBFS，避免削顶
    if not samples:
        return []
    mean = sum(samples) / len(samples)
    samples = [s - mean for s in samples]
    peak = max(max(samples), -min(samples)) or 1
    gain = (32767 * 0.25) / peak
    pcm = [max(-32768, min(32767, int(s * gain))) for s in samples]
    return [linear2ulaw(s) for s in pcm]


def write_outputs(clips):
    offset = 0
    metadata = []
    with open(OUT_BIN, "wb") as f:
        for name, samples in clips:
            f.write(bytes(samples))
            metadata.append((name, offset, len(samples)))
            offset += len(samples)

    lines = [
        "/* 本文件由 tools/gen_voice.py 生成，请勿手改。 */",
        "",
        '#include "voice_data.h"',
        "",
        "extern const uint8_t _binary_voice_data_bin_start[];",
        "",
    ]
    lines.append("const voice_clip_t g_voice_clips[VOICE_MAX] = {")
    for idx, (name, start, length) in enumerate(metadata):
        lines.append(
            "    [%d] = { .ulaw = _binary_voice_data_bin_start + %d,"
            " .samples = %d },"
            % (idx, start, length)
        )
    lines.append("};")
    lines.append("")
    with open(OUT_C, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))
    return OUT_C, OUT_BIN


def main():
    os.makedirs(PREVIEW_DIR, exist_ok=True)
    result = []
    total_bytes = 0
    for name, text in CLIPS:
        wav = os.path.join(PREVIEW_DIR, name + ".wav")
        synth(text, wav)
        data, rate, ch, bits = read_pcm(wav)
        samples = to_mono8k_ulaw(data, rate, ch, bits)
        total_bytes += len(samples)
        print("%-18s %5.2fs  %6d samples  (%s)" %
              (name, len(samples) / RATE, len(samples), text))
        result.append((name, samples))

    out_c, out_bin = write_outputs(result)
    print("\n已写入 %s" % out_c)
    print("已写入 %s" % out_bin)
    print("语音总大小 %.1f KB（app 分区 4MB 无压力）"
          % (total_bytes / 1024.0))


if __name__ == "__main__":
    sys.exit(main())
