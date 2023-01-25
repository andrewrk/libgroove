const std = @import("std");

pub fn build(b: *std.build.Builder) void {
    const target = b.standardTargetOptions(.{});
    const mode = b.standardReleaseOptions();

    const ffmpeg_dep = b.dependency("ffmpeg", .{});
    const soundio_dep = b.dependency("soundio", .{});
    const ebur128_dep = b.dependency("ebur128", .{});
    const chromaprint_dep = b.dependency("chromaprint", .{});

    const libsoundio = soundio_dep.artifact("soundio");

    const groove = b.addStaticLibrary("groove", null);
    groove.setTarget(target);
    groove.setBuildMode(mode);
    groove.addIncludePath(".");
    groove.addCSourceFiles(&.{
        "src/buffer.c",
        "src/file.c",
        "src/groove.c",
        "src/player.c",
        "src/queue.c",
        "src/encoder.c",
        "src/fingerprinter.c",
        "src/loudness.c",
        "src/waveform.c",
        "src/playlist.c",
        "src/util.c",
        "src/os.c",
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
    groove.linkLibrary(ffmpeg_dep.artifact("ffmpeg"));
    groove.linkLibrary(chromaprint_dep.artifact("chromaprint"));
    groove.linkLibrary(ebur128_dep.artifact("ebur128"));
    groove.linkLibrary(libsoundio);
    groove.linkLibC();
    groove.install();
    groove.installHeadersDirectory("groove", "groove");
    groove.installLibraryHeaders(libsoundio);

    const playlist = b.addExecutable("playlist", null);
    playlist.setTarget(target);
    playlist.setBuildMode(mode);
    playlist.addCSourceFiles(&.{
        "example/playlist.c",
    }, example_cflags);
    playlist.linkLibrary(groove);
    playlist.install();

    const metadata = b.addExecutable("metadata", null);
    metadata.setTarget(target);
    metadata.setBuildMode(mode);
    metadata.addCSourceFiles(&.{
        "example/metadata.c",
    }, example_cflags);
    metadata.linkLibrary(groove);
    metadata.install();
}

const example_cflags: []const []const u8 = &.{
    "-std=c99", "-pedantic", "-Werror", "-Wall",
};
