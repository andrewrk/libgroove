const std = @import("std");

pub fn build(b: *std.build.Builder) void {
    const target = b.standardTargetOptions(.{});
    const mode = b.standardReleaseOptions();

    const ffmpeg = b.addStaticLibrary("ffmpeg", null);
    ffmpeg.setTarget(target);
    ffmpeg.setBuildMode(mode);
    ffmpeg.linkLibC();
    ffmpeg.addCSourceFiles(&.{}, &.{});

    const soundio = b.addStaticLibrary("soundio", null);
    soundio.setTarget(target);
    soundio.setBuildMode(mode);
    soundio.linkLibC();
    soundio.addIncludeDir("deps/soundio");
    soundio.addCSourceFiles(&.{
        "deps/soundio/src/soundio.c",
        "deps/soundio/src/util.c",
        "deps/soundio/src/os.c",
        "deps/soundio/src/dummy.c",
        "deps/soundio/src/channel_layout.c",
        "deps/soundio/src/ring_buffer.c",
    }, &.{
        "-std=c11",
        "-fvisibility=hidden",
        "-Wall",
        "-Werror=strict-prototypes",
        "-Werror=old-style-definition",
        "-Werror=missing-prototypes",
        "-D_REENTRANT",
        "-D_POSIX_C_SOURCE=200809L",
        "-Wno-missing-braces",
    });

    const ebur128 = b.addStaticLibrary("ebur128", null);
    ebur128.setTarget(target);
    ebur128.setBuildMode(mode);
    ebur128.linkLibC();
    ebur128.linkSystemLibrary("m");
    ebur128.addCSourceFiles(&.{
        "deps/ebur128/ebur128/ebur128.c",
    }, &.{});

    const chromaprint = b.addStaticLibrary("chromaprint", null);
    chromaprint.setTarget(target);
    chromaprint.setBuildMode(mode);
    chromaprint.linkLibC();
    chromaprint.linkSystemLibrary("c++");
    chromaprint.addIncludeDir("deps/chromaprint/src");
    chromaprint.addIncludeDir("deps/ffmpeg");
    chromaprint.addCSourceFiles(&.{
        "deps/chromaprint/src/audio_processor.cpp",
        "deps/chromaprint/src/chroma.cpp",
        "deps/chromaprint/src/chroma_resampler.cpp",
        "deps/chromaprint/src/chroma_filter.cpp",
        "deps/chromaprint/src/spectrum.cpp",
        "deps/chromaprint/src/fft.cpp",
        "deps/chromaprint/src/fingerprinter.cpp",
        "deps/chromaprint/src/image_builder.cpp",
        "deps/chromaprint/src/simhash.cpp",
        "deps/chromaprint/src/silence_remover.cpp",
        "deps/chromaprint/src/fingerprint_calculator.cpp",
        "deps/chromaprint/src/fingerprint_compressor.cpp",
        "deps/chromaprint/src/fingerprint_decompressor.cpp",
        "deps/chromaprint/src/fingerprinter_configuration.cpp",
        "deps/chromaprint/src/fingerprint_matcher.cpp",
        "deps/chromaprint/src/utils/base64.cpp",
        "deps/chromaprint/src/chromaprint.cpp",
        "deps/chromaprint/src/fft_lib_avfft.cpp",
    }, &.{
        "-std=c++11",
        "-DHAVE_CONFIG_H",
        "-D_SCL_SECURE_NO_WARNINGS",
        "-D__STDC_LIMIT_MACROS",
        "-D__STDC_CONSTANT_MACROS",
        "-DCHROMAPRINT_NODLL",
    });
    chromaprint.addCSourceFiles(&.{
        "deps/chromaprint/src/avresample/resample2.c",
    }, &.{
        "-std=c11",
        "-DHAVE_CONFIG_H",
        "-D_SCL_SECURE_NO_WARNINGS",
        "-D__STDC_LIMIT_MACROS",
        "-D__STDC_CONSTANT_MACROS",
        "-DCHROMAPRINT_NODLL",
        "-D_GNU_SOURCE",
    });

    //const transcode = b.addExecutable("transcode", null);
    //transcode.setTarget(target);
    //transcode.setBuildMode(mode);
    //transcode.addCSourceFiles(&.{
    //}, &.{
    //});

    const groove = b.addStaticLibrary("groove", null);
    groove.setTarget(target);
    groove.setBuildMode(mode);
    groove.linkLibrary(ffmpeg);
    groove.linkLibrary(chromaprint);
    groove.linkLibrary(ebur128);
    groove.linkLibrary(soundio);
    groove.linkLibC();
    groove.addIncludeDir(".");
    groove.addIncludeDir("deps");
    groove.addIncludeDir("deps/ffmpeg");
    groove.addIncludeDir("deps/ebur128/ebur128");
    groove.addIncludeDir("deps/soundio");
    groove.addCSourceFiles(&.{
        "groove/buffer.c",
        "groove/encoder.c",
        "groove/file.c",
        "groove/fingerprinter.c",
        "groove/global.c",
        "groove/loudness.c",
        "groove/player.c",
        "groove/playlist.c",
        "groove/queue.c",
    }, &.{
        "-std=c99",
        "-pedantic",
        "-Wall",
        "-Werror=strict-prototypes",
        "-Werror=old-style-definition",
        "-Werror=missing-prototypes",
        "-D_REENTRANT",
        "-D_POSIX_C_SOURCE=200809L",
    });

    const playlist = b.addExecutable("playlist", null);
    playlist.setTarget(target);
    playlist.setBuildMode(mode);
    playlist.linkLibrary(groove);
    playlist.addIncludeDir(".");
    playlist.addCSourceFiles(&.{
        "example/playlist.c",
    }, example_cflags);
    playlist.install();
}

const example_cflags: []const []const u8 = &.{
    "-std=c99", "-pedantic", "-Werror", "-Wall",
};
