/*
 * Copyright (c) 2015 Andrew Kelley
 *
 * This file is part of libgroove, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#ifndef GROOVE_UTIL_HPP
#define GROOVE_UTIL_HPP

#include "ffmpeg.hpp"
#include "groove_private.h"

#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define BREAKPOINT __asm("int $0x03")

template<typename T>
__attribute__((malloc)) static inline T *allocate_nonzero(size_t count) {
    if (count >= INT_MAX / sizeof(T))
        return nullptr;
    return reinterpret_cast<T*>(av_malloc(count * sizeof(T)));
}

template<typename T>
__attribute__((malloc)) static inline T *allocate(size_t count) {
    if (count >= INT_MAX / sizeof(T))
        return nullptr;
    return reinterpret_cast<T*>(av_mallocz(count * sizeof(T)));
}

template<typename T>
static inline T *reallocate_nonzero(T * old, size_t new_count) {
    if (new_count >= INT_MAX / sizeof(T))
        return nullptr;
    return reinterpret_cast<T*>(av_realloc(old, new_count * sizeof(T)));
}

template<typename T>
static inline void deallocate(T *ptr) {
    av_free(ptr);
}

void groove_panic(const char *format, ...)
    __attribute__((cold))
    __attribute__ ((noreturn))
    __attribute__ ((format (printf, 1, 2)));

template <typename T, long n>
constexpr long array_length(const T (&)[n]) {
    return n;
}

template <typename T>
static inline T max(T a, T b) {
    return (a >= b) ? a : b;
}

template <typename T>
static inline T min(T a, T b) {
    return (a <= b) ? a : b;
}

template<typename T>
static inline T clamp(T min_value, T value, T max_value) {
    return max(min(value, max_value), min_value);
}

enum SoundIoChannelId from_ffmpeg_channel_id(uint64_t ffmpeg_channel_id);
void from_ffmpeg_layout(uint64_t in_layout, SoundIoChannelLayout *out_layout);
enum SoundIoFormat from_ffmpeg_format(AVSampleFormat fmt);
bool from_ffmpeg_format_planar(AVSampleFormat fmt);

uint64_t to_ffmpeg_channel_id(SoundIoChannelId channel_id);
uint64_t to_ffmpeg_channel_layout(const SoundIoChannelLayout *channel_layout);
AVSampleFormat to_ffmpeg_fmt(const GrooveAudioFormat *fmt);
AVSampleFormat to_ffmpeg_fmt_params(enum SoundIoFormat format, bool is_planar);

#endif
