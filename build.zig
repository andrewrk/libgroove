const std = @import("std");

pub fn build(b: *std.build.Builder) void {
    const target = b.standardTargetOptions(.{});
    const mode = b.standardReleaseOptions();

    const ffmpeg = b.addStaticLibrary("ffmpeg", null);
    ffmpeg.setTarget(target);
    ffmpeg.setBuildMode(mode);
    ffmpeg.linkLibC();
    ffmpeg.addCSourceFiles(&.{}, &.{});

    //const sdl2 = b.addStaticLibrary("SDL2", null);
    //sdl2.setTarget(target);
    //sdl2.setBuildMode(mode);
    //sdl2.linkLibC();
    //sdl2.addCSourceFiles(&.{
    //}, &.{
    //});

    //const ebur128 = b.addStaticLibrary("ebur128", null);
    //ebur128.setTarget(target);
    //ebur128.setBuildMode(mode);
    //ebur128.linkLibC();
    //ebur128.addCSourceFiles(&.{
    //}, &.{
    //});

    //const chromaprint = b.addStaticLibrary("chromaprint", null);
    //chromaprint.setTarget(target);
    //chromaprint.setBuildMode(mode);
    //chromaprint.linkLibC();
    //chromaprint.addCSourceFiles(&.{
    //}, &.{
    //});

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
    groove.linkLibC();
    groove.addIncludeDir(".");
    groove.addIncludeDir("deps/ffmpeg");
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
