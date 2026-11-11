# -*- coding: utf-8 -*-
"""
把中文提示语合成成固件可用的 PCM16 数组。

为什么要走这一步：
  设备断网时没法联网，语音提示必须**内置在固件里**。
  esp_audio_simple_player 走 URI 拉流，本地文件协议支持不确定；
  直接把 16kHz/16bit/单声道 PCM 塞进 .rodata，I2S 一推就响，零解码依赖。

用法：
  python tools/gen_voice.py

输出：
  main/voice_data.c      （生成的数组，别手改）
  tools/voice_preview/   （合成好的 wav，方便先试听）

改了文案就重跑一次，然后 idf.py build。
"""

import os
import struct
import sys
import wave

import win32com.client

# 采样率固定 16k：体积只有 22k 的七成，播报人声完全够用。
RATE = 16000

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
PREVIEW_DIR = os.path.join(HERE, "voice_preview")


def synth(text, path):
    """SAPI 合成到 wav。优先让引擎直接吐 16k/16bit/单声道。"""
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


def to_mono16k(data, rate, ch, bits):
    """统一成 16kHz / 16bit / 单声道。SAPI 已给 16k 时这里只是去立体声。"""
    if bits != 16:
        raise SystemExit("只支持 16bit 输出，实际 %d" % bits)

    samples = list(struct.unpack("<%dh" % (len(data) // 2), data))
    if ch == 2:
        samples = [(samples[i] + samples[i + 1]) // 2
                   for i in range(0, len(samples) - 1, 2)]

    if rate != RATE:
        # 用线性插值重采样就够了：语音提示不追求发烧级音质
        ratio = RATE / float(rate)
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
    return [max(-32768, min(32767, int(s * gain))) for s in samples]


def write_c(clips):
    lines = [
        "/* 本文件由 tools/gen_voice.py 生成，请勿手改。 */",
        "",
        '#include "voice_data.h"',
        "",
    ]
    for name, samples in clips:
        lines.append("const int16_t %s[] = {" % name)
        for i in range(0, len(samples), 16):
            row = ", ".join(str(s) for s in samples[i:i + 16])
            lines.append("    " + row + ",")
        lines.append("};")
        lines.append("const size_t %s_len = %d;" % (name, len(samples)))
        lines.append("")

    lines.append("const voice_clip_t g_voice_clips[VOICE_MAX] = {")
    for idx, (name, samples) in enumerate(clips):
        lines.append("    [%d] = { .pcm = %s, .samples = %s_len },"
                     % (idx, name, name))
    lines.append("};")
    lines.append("")
    with open(OUT_C, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))
    return OUT_C


def main():
    os.makedirs(PREVIEW_DIR, exist_ok=True)
    result = []
    total_bytes = 0
    for name, text in CLIPS:
        wav = os.path.join(PREVIEW_DIR, name + ".wav")
        synth(text, wav)
        data, rate, ch, bits = read_pcm(wav)
        samples = to_mono16k(data, rate, ch, bits)
        total_bytes += len(samples) * 2
        print("%-18s %5.2fs  %6d samples  (%s)" %
              (name, len(samples) / RATE, len(samples), text))
        result.append((name, samples))

    out = write_c(result)
    print("\n已写入 %s" % out)
    print("语音总大小 %.1f KB（约 1.2 秒/10KB，app 分区 4MB 无压力）"
          % (total_bytes / 1024.0))


if __name__ == "__main__":
    sys.exit(main())
