const std = @import("std");

pub const HostApi = enum {
    alsa,
    asihpi,
    asio,
    coreaudio,
    dsound,
    jack,
    oss,
    pulseaudio,
    wasapi,
    wdmks,
    wmme,
    aaudio,
    opensles,

    pub const defaults = struct {
        pub const macos: []const HostApi = &.{.coreaudio};
        pub const linux: []const HostApi = &.{ .alsa, .pulseaudio };
        pub const windows: []const HostApi = &.{.wasapi};
        pub const android: []const HostApi = &.{ .aaudio, .opensles };
    };
};

fn unsupportedOs(os: std.Target.Os.Tag) noreturn {
    std.log.err("unsupported OS: {s}", .{@tagName(os)});
    std.process.exit(1);
}

fn unsupportedHostApi(os: std.Target.Os.Tag, api: HostApi) noreturn {
    std.log.err("host API {s} is unsupported on {s}", .{ @tagName(api), @tagName(os) });
    std.process.exit(1);
}

pub fn build(b: *std.Build) !void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    const shared = b.option(bool, "shared", "Create shared library instead of static") orelse false;
    const host_api_opts = b.option([]const HostApi, "host-api", "Enable specific host audio APIs");
    const paths: SystemPaths = .{
        .include = b.option(std.Build.LazyPath, "system_include_path", "system include path"),
        .framework = b.option(std.Build.LazyPath, "system_framework_path", "system framework path"),
        .library = b.option(std.Build.LazyPath, "library_path", "system library path"),
    };

    const lib = try setupLib(b, target, optimize, shared, host_api_opts, paths);
    b.installArtifact(lib);

    const portaudio_mod = b.addModule("portaudio", .{
        .root_source_file = b.path("portaudio.zig"),
        .target = target,
        .optimize = optimize,
    });
    portaudio_mod.linkLibrary(lib);
    if (target.result.abi.isAndroid()) {
        if (paths.library) |p| portaudio_mod.addLibraryPath(p);
        portaudio_mod.linkSystemLibrary("OpenSLES", .{ .use_pkg_config = .no });
        portaudio_mod.linkSystemLibrary("dl", .{ .use_pkg_config = .no });
    }

    const test_step = b.step("test", "Run tests");
    test_step.dependOn(&b.addRunArtifact(b.addTest(.{ .root_module = portaudio_mod })).step);
}

const SystemPaths = struct {
    include: ?std.Build.LazyPath,
    framework: ?std.Build.LazyPath,
    library: ?std.Build.LazyPath,
};

fn setupLib(
    b: *std.Build,
    t: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
    shared: bool,
    host_api_opts: ?[]const HostApi,
    paths: SystemPaths,
) !*std.Build.Step.Compile {
    const pa_root = b.path(".");

    const lib_mod = b.createModule(.{
        .target = t,
        .optimize = optimize,
        .link_libc = true,
    });
    const lib = b.addLibrary(.{
        .name = "portaudio",
        .root_module = lib_mod,
        .linkage = if (shared) .dynamic else .static,
    });

    if (paths.include) |p| lib_mod.addSystemIncludePath(p);
    if (paths.framework) |p| lib_mod.addSystemFrameworkPath(p);
    if (paths.library) |p| lib_mod.addLibraryPath(p);
    const pkg_config: std.Build.Module.SystemLib.UsePkgConfig = if (t.query.isNative()) .yes else .no;

    lib_mod.addCMacro(if (t.result.cpu.arch.endian() == .little) "PA_LITTLE_ENDIAN" else "PA_BIG_ENDIAN", "1");
    lib_mod.addIncludePath(b.path("include"));
    lib_mod.addIncludePath(b.path("src/common"));
    lib.installHeadersDirectory(b.path("include"), "", .{});
    lib_mod.addCSourceFiles(.{ .root = pa_root, .files = src_common });

    const host_apis = host_api_opts orelse switch (t.result.os.tag) {
        .macos => HostApi.defaults.macos,
        .linux => if (t.result.abi.isAndroid()) HostApi.defaults.android else HostApi.defaults.linux,
        .windows => HostApi.defaults.windows,
        else => unsupportedOs(t.result.os.tag),
    };

    var flags = std.ArrayList([]const u8).empty;
    defer flags.deinit(b.allocator);

    switch (t.result.os.tag) {
        .macos => {
            for (host_apis) |api| {
                switch (api) {
                    .coreaudio => {
                        try flags.append(b.allocator, "-DPA_USE_COREAUDIO=1");
                        lib_mod.addIncludePath(b.path("src/hostapi/coreaudio"));
                        lib_mod.addCSourceFiles(.{ .root = pa_root, .files = src_hostapi_coreaudio });
                        lib_mod.linkFramework("AudioToolbox", .{});
                        lib_mod.linkFramework("AudioUnit", .{});
                        lib_mod.linkFramework("CoreAudio", .{});
                        lib_mod.linkFramework("CoreServices", .{});
                    },
                    else => unsupportedHostApi(t.result.os.tag, api),
                }
            }
            lib_mod.addIncludePath(b.path("src/os/unix"));
            lib_mod.addCSourceFiles(.{ .root = pa_root, .files = src_os_unix, .flags = flags.items });
        },
        .linux => if (t.result.abi.isAndroid()) {
            lib_mod.pic = true;
            for (host_apis) |api| {
                switch (api) {
                    .aaudio => {
                        try flags.append(b.allocator, "-DPA_USE_AAUDIO=1");
                        lib_mod.addCSourceFiles(.{ .root = pa_root, .files = src_hostapi_aaudio });
                    },
                    .opensles => {
                        try flags.append(b.allocator, "-DPA_USE_OPENSLES=1");
                        lib_mod.addCSourceFiles(.{ .root = pa_root, .files = src_hostapi_opensles });
                    },
                    else => unsupportedHostApi(t.result.os.tag, api),
                }
            }
            lib_mod.addIncludePath(b.path("src/os/unix"));
            lib_mod.addCSourceFiles(.{ .root = pa_root, .files = src_os_unix, .flags = flags.items });
        } else {
            for (host_apis) |api| {
                switch (api) {
                    .alsa => {
                        try flags.append(b.allocator, "-DPA_USE_ALSA=1");
                        lib_mod.addIncludePath(b.path("src/hostapi/alsa"));
                        lib_mod.addCSourceFiles(.{ .root = pa_root, .files = src_hostapi_alsa });
                        lib_mod.linkSystemLibrary("asound", .{ .use_pkg_config = pkg_config });
                    },
                    .pulseaudio => {
                        try flags.append(b.allocator, "-DPA_USE_PULSEAUDIO=1");
                        lib_mod.addIncludePath(b.path("src/hostapi/pulseaudio"));
                        lib_mod.addCSourceFiles(.{ .root = pa_root, .files = src_hostapi_pulseaudio });
                        lib_mod.linkSystemLibrary("pulse", .{ .use_pkg_config = pkg_config });
                    },
                    .jack => {
                        try flags.append(b.allocator, "-DPA_USE_JACK=1");
                        lib_mod.addIncludePath(b.path("src/hostapi/jack"));
                        lib_mod.addCSourceFiles(.{ .root = pa_root, .files = src_hostapi_jack });
                        lib_mod.linkSystemLibrary("jack", .{ .use_pkg_config = pkg_config });
                    },
                    .oss => {
                        try flags.append(b.allocator, "-DPA_USE_OSS=1");
                        lib_mod.addIncludePath(b.path("src/hostapi/oss"));
                        lib_mod.addCSourceFiles(.{ .root = pa_root, .files = src_hostapi_oss });
                    },
                    else => unsupportedHostApi(t.result.os.tag, api),
                }
            }
            lib_mod.addIncludePath(b.path("src/os/unix"));
            lib_mod.addCSourceFiles(.{ .root = pa_root, .files = src_os_unix, .flags = flags.items });
        },
        .windows => {
            for (host_apis) |api| {
                switch (api) {
                    .wasapi => {
                        try flags.append(b.allocator, "-DPA_USE_WASAPI=1");
                        lib_mod.addIncludePath(b.path("src/hostapi/wasapi"));
                        lib_mod.addCSourceFiles(.{ .root = pa_root, .files = src_hostapi_wasapi });
                    },
                    .dsound => {
                        try flags.append(b.allocator, "-DPA_USE_DS=1");
                        lib_mod.addIncludePath(b.path("src/hostapi/dsound"));
                        lib_mod.addCSourceFiles(.{ .root = pa_root, .files = src_hostapi_dsound });
                    },
                    .wmme => {
                        try flags.append(b.allocator, "-DPA_USE_WMME=1");
                        lib_mod.addIncludePath(b.path("src/hostapi/wmme"));
                        lib_mod.addCSourceFiles(.{ .root = pa_root, .files = src_hostapi_wmme });
                    },
                    else => unsupportedHostApi(t.result.os.tag, api),
                }
            }
            lib_mod.addIncludePath(b.path("src/os/win"));
            lib_mod.addCSourceFiles(.{ .root = pa_root, .files = src_os_win, .flags = flags.items });
            lib_mod.linkSystemLibrary("winmm", .{});
            lib_mod.linkSystemLibrary("ole32", .{});
        },
        else => unsupportedOs(t.result.os.tag),
    }

    return lib;
}

const src_common = &.{
    "src/common/pa_allocation.c",
    "src/common/pa_converters.c",
    "src/common/pa_cpuload.c",
    "src/common/pa_debugprint.c",
    "src/common/pa_dither.c",
    "src/common/pa_front.c",
    "src/common/pa_process.c",
    "src/common/pa_ringbuffer.c",
    "src/common/pa_stream.c",
    "src/common/pa_trace.c",
};

const src_os_unix = &.{
    "src/os/unix/pa_pthread_util.c",
    "src/os/unix/pa_unix_hostapis.c",
    "src/os/unix/pa_unix_util.c",
};

const src_os_win = &.{
    "src/os/win/pa_win_coinitialize.c",
    "src/os/win/pa_win_hostapis.c",
    "src/os/win/pa_win_util.c",
    "src/os/win/pa_win_version.c",
    "src/os/win/pa_win_waveformat.c",
    "src/os/win/pa_win_wdmks_utils.c",
    "src/os/win/pa_x86_plain_converters.c",
};

const src_hostapi_aaudio = &.{
    "src/hostapi/aaudio/pa_aaudio.c",
};

const src_hostapi_opensles = &.{
    "src/hostapi/opensles/pa_opensles.c",
};

const src_hostapi_alsa = &.{
    "src/hostapi/alsa/pa_linux_alsa.c",
};

const src_hostapi_asihpi = &.{
    "src/hostapi/asihpi/pa_linux_asihpi.c",
};

const src_hostapi_coreaudio = &.{
    "src/hostapi/coreaudio/pa_mac_core.c",
    "src/hostapi/coreaudio/pa_mac_core_blocking.c",
    "src/hostapi/coreaudio/pa_mac_core_utilities.c",
};

const src_hostapi_dsound = &.{
    "src/hostapi/dsound/pa_win_ds.c",
    "src/hostapi/dsound/pa_win_ds_dynlink.c",
};

const src_hostapi_jack = &.{
    "src/hostapi/jack/pa_jack.c",
};

const src_hostapi_oss = &.{
    "src/hostapi/oss/pa_unix_oss.c",
};

const src_hostapi_pulseaudio = &.{
    "src/hostapi/pulseaudio/pa_linux_pulseaudio.c",
    "src/hostapi/pulseaudio/pa_linux_pulseaudio_block.c",
    "src/hostapi/pulseaudio/pa_linux_pulseaudio_cb.c",
};

const src_hostapi_wasapi = &.{
    "src/hostapi/wasapi/pa_win_wasapi.c",
};

const src_hostapi_wdmks = &.{
    "src/hostapi/wdmks/pa_win_wdmks.c",
};

const src_hostapi_wmme = &.{
    "src/hostapi/wmme/pa_win_wmme.c",
};
