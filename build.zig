const std = @import("std");

pub fn build(b: *std.build.Builder) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const ffmpeg_dep = b.dependency("ffmpeg", .{
        .target = target,
        .optimize = optimize,
    });
    const soundio_dep = b.dependency("soundio", .{
        .target = target,
        .optimize = optimize,
    });
    const ebur128_dep = b.dependency("ebur128", .{
        .target = target,
        .optimize = optimize,
    });
    const chromaprint_dep = b.dependency("chromaprint", .{
        .target = target,
        .optimize = optimize,
    });

    const libsoundio = soundio_dep.artifact("soundio");

    const groove = b.addStaticLibrary(.{
        .name = "groove",
        .target = target,
        .optimize = optimize,
    });
    groove.addIncludePath(.{ .path = "." });
    groove.addCSourceFiles(.{
        .files = &.{
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
        },
        .flags = &.{
            "-std=c99",
            "-pedantic",
            "-Wall",
            "-Werror=strict-prototypes",
            "-Werror=old-style-definition",
            "-Werror=missing-prototypes",
            "-D_REENTRANT",
            "-D_POSIX_C_SOURCE=200809L",
        },
    });
    groove.linkLibrary(ffmpeg_dep.artifact("ffmpeg"));
    groove.linkLibrary(chromaprint_dep.artifact("chromaprint"));
    groove.linkLibrary(ebur128_dep.artifact("ebur128"));
    groove.linkLibrary(libsoundio);
    groove.linkLibC();
    groove.installHeadersDirectory("groove", "groove");
    groove.installLibraryHeaders(libsoundio);
    b.installArtifact(groove);

    const playlist = b.addExecutable(.{
        .name = "playlist",
        .target = target,
        .optimize = optimize,
    });
    playlist.addCSourceFiles(.{
        .files = &.{
            "example/playlist.c",
        },
        .flags = example_cflags,
    });
    playlist.linkLibrary(groove);
    b.installArtifact(playlist);

    const metadata = b.addExecutable(.{
        .name = "metadata",
        .target = target,
        .optimize = optimize,
    });
    metadata.addCSourceFiles(.{
        .files = &.{
            "example/metadata.c",
        },
        .flags = example_cflags,
    });
    metadata.linkLibrary(groove);
    b.installArtifact(metadata);
}

const example_cflags: []const []const u8 = &.{
    "-std=c99", "-pedantic", "-Werror", "-Wall",
};
