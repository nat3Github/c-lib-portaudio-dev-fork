const builtin = @import("builtin");
const std = @import("std");
const assert = std.debug.assert;
const expect = std.debug.expect;
const panic = std.debug.panic;

pub const c = struct {
    pub const PaError = c_int;
    pub const paNoError: PaError = 0;
    pub const paNotInitialized: PaError = -10000;
    pub const paUnanticipatedHostError: PaError = -9999;
    pub const paInvalidChannelCount: PaError = -9998;
    pub const paInvalidSampleRate: PaError = -9997;
    pub const paInvalidDevice: PaError = -9996;
    pub const paInvalidFlag: PaError = -9995;
    pub const paSampleFormatNotSupported: PaError = -9994;
    pub const paBadIODeviceCombination: PaError = -9993;
    pub const paInsufficientMemory: PaError = -9992;
    pub const paBufferTooBig: PaError = -9991;
    pub const paBufferTooSmall: PaError = -9990;
    pub const paNullCallback: PaError = -9989;
    pub const paBadStreamPtr: PaError = -9988;
    pub const paTimedOut: PaError = -9987;
    pub const paInternalError: PaError = -9986;
    pub const paDeviceUnavailable: PaError = -9985;
    pub const paIncompatibleHostApiSpecificStreamInfo: PaError = -9984;
    pub const paStreamIsStopped: PaError = -9983;
    pub const paStreamIsNotStopped: PaError = -9982;
    pub const paInputOverflowed: PaError = -9981;
    pub const paOutputUnderflowed: PaError = -9980;
    pub const paHostApiNotFound: PaError = -9979;
    pub const paInvalidHostApi: PaError = -9978;
    pub const paCanNotReadFromACallbackStream: PaError = -9977;
    pub const paCanNotWriteToACallbackStream: PaError = -9976;
    pub const paCanNotReadFromAnOutputOnlyStream: PaError = -9975;
    pub const paCanNotWriteToAnInputOnlyStream: PaError = -9974;
    pub const paIncompatibleStreamHostApi: PaError = -9973;
    pub const paBadBufferPtr: PaError = -9972;
    pub const paCanNotInitializeRecursively: PaError = -9971;

    pub const PaTime = f64;
    pub const PaDeviceIndex = c_int;
    pub const PaHostApiIndex = c_int;
    pub const PaHostApiTypeId = c_int;
    pub const PaSampleFormat = c_ulong;
    pub const paFloat32: PaSampleFormat = 0x00000001;
    pub const paInt32: PaSampleFormat = 0x00000002;
    pub const paInt24: PaSampleFormat = 0x00000004;
    pub const paInt16: PaSampleFormat = 0x00000008;
    pub const paInt8: PaSampleFormat = 0x00000010;

    pub const PaStreamFlags = c_ulong;
    pub const paNoFlag: PaStreamFlags = 0;

    pub const PaStreamCallbackFlags = c_ulong;

    pub const paContinue: c_int = 0;
    pub const paComplete: c_int = 1;
    pub const paAbort: c_int = 2;

    pub const PaStream = opaque {};

    pub const struct_PaHostApiInfo = extern struct {
        structVersion: c_int,
        type: PaHostApiTypeId,
        name: [*c]const u8,
        deviceCount: c_int,
        defaultInputDevice: PaDeviceIndex,
        defaultOutputDevice: PaDeviceIndex,
    };
    pub const PaHostApiInfo = struct_PaHostApiInfo;

    pub const struct_PaDeviceInfo = extern struct {
        structVersion: c_int,
        name: [*c]const u8,
        hostApi: PaHostApiIndex,
        maxInputChannels: c_int,
        maxOutputChannels: c_int,
        defaultLowInputLatency: PaTime,
        defaultLowOutputLatency: PaTime,
        defaultHighInputLatency: PaTime,
        defaultHighOutputLatency: PaTime,
        defaultSampleRate: f64,
    };
    pub const PaDeviceInfo = struct_PaDeviceInfo;

    pub const PaStreamParameters = extern struct {
        device: PaDeviceIndex,
        channelCount: c_int,
        sampleFormat: PaSampleFormat,
        suggestedLatency: PaTime,
        hostApiSpecificStreamInfo: ?*anyopaque,
    };

    pub const PaStreamCallbackTimeInfo = extern struct {
        inputBufferAdcTime: PaTime,
        currentTime: PaTime,
        outputBufferDacTime: PaTime,
    };

    pub const struct_PaStreamInfo = extern struct {
        structVersion: c_int,
        inputLatency: PaTime,
        outputLatency: PaTime,
        sampleRate: f64,
    };
    pub const PaStreamInfo = struct_PaStreamInfo;

    pub const PaStreamCallback = *const fn (
        input: ?*const anyopaque,
        output: ?*anyopaque,
        frameCount: c_ulong,
        timeInfo: [*c]const PaStreamCallbackTimeInfo,
        statusFlags: PaStreamCallbackFlags,
        userData: ?*anyopaque,
    ) callconv(.c) c_int;

    pub extern fn Pa_Initialize() PaError;
    pub extern fn Pa_Terminate() PaError;
    pub extern fn Pa_GetHostApiCount() PaHostApiIndex;
    pub extern fn Pa_GetHostApiInfo(hostApi: PaHostApiIndex) ?*const PaHostApiInfo;
    pub extern fn Pa_GetDeviceCount() PaDeviceIndex;
    pub extern fn Pa_GetDeviceInfo(device: PaDeviceIndex) ?*const PaDeviceInfo;
    pub extern fn Pa_GetDefaultInputDevice() PaDeviceIndex;
    pub extern fn Pa_GetDefaultOutputDevice() PaDeviceIndex;
    pub extern fn Pa_OpenStream(
        stream: *?*PaStream,
        inputParameters: ?*const PaStreamParameters,
        outputParameters: ?*const PaStreamParameters,
        sampleRate: f64,
        framesPerBuffer: c_ulong,
        streamFlags: PaStreamFlags,
        streamCallback: ?PaStreamCallback,
        userData: ?*anyopaque,
    ) PaError;
    pub extern fn Pa_StartStream(stream: *PaStream) PaError;
    pub extern fn Pa_StopStream(stream: *PaStream) PaError;
    pub extern fn Pa_AbortStream(stream: *PaStream) PaError;
    pub extern fn Pa_CloseStream(stream: *PaStream) PaError;
    pub extern fn Pa_IsStreamActive(stream: *PaStream) PaError;
    pub extern fn Pa_IsStreamStopped(stream: *PaStream) PaError;
    pub extern fn Pa_GetStreamInfo(stream: *PaStream) ?*const PaStreamInfo;
    pub extern fn Pa_GetStreamTime(stream: *PaStream) PaTime;
    pub extern fn Pa_GetStreamCpuLoad(stream: *PaStream) f64;
};

pub const DeviceInfo = c.PaDeviceInfo;

pub const PortAudio = @This();
fn errify_print(err: c.PaError) !void {
    errify(err) catch |e| {
        std.log.err("error: {}", .{e});
        return e;
    };
}

fn errify(err: c.PaError) !void {
    if (err < 0) {
        switch (err) {
            c.paNoError => {},
            c.paNotInitialized => return error.PortAudioNotInitialized,
            c.paUnanticipatedHostError => return error.PortAudioUnanticipatedHostError,
            c.paInvalidChannelCount => return error.PortAudioInvalidChannelCount,
            c.paInvalidSampleRate => return error.PortAudioInvalidSampleRate,
            c.paInvalidDevice => return error.PortAudioInvalidDevice,
            c.paInvalidFlag => return error.PortAudioInvalidFlag,
            c.paSampleFormatNotSupported => return error.PortAudioSampleFormatNotSupported,
            c.paBadIODeviceCombination => return error.PortAudioBadIODeviceCombination,
            c.paInsufficientMemory => return error.PortAudioInsufficientMemory,
            c.paBufferTooBig => return error.PortAudioBufferTooBig,
            c.paBufferTooSmall => return error.PortAudioBufferTooSmall,
            c.paNullCallback => return error.PortAudioNullCallback,
            c.paBadStreamPtr => return error.PortAudioBadStreamPtr,
            c.paTimedOut => return error.PortAudioTimedOut,
            c.paInternalError => return error.PortAudioInternalError,
            c.paDeviceUnavailable => return error.PortAudioDeviceUnavailable,
            c.paIncompatibleHostApiSpecificStreamInfo => return error.PortAudioIncompatibleHostApiSpecificStreamInfo,
            c.paStreamIsStopped => return error.PortAudioStreamIsStopped,
            c.paStreamIsNotStopped => return error.PortAudioStreamIsNotStopped,
            c.paInputOverflowed => return error.PortAudioInputOverflowed,
            c.paOutputUnderflowed => return error.PortAudioOutputUnderflowed,
            c.paHostApiNotFound => return error.PortAudioHostApiNotFound,
            c.paInvalidHostApi => return error.PortAudioInvalidHostApi,
            c.paCanNotReadFromACallbackStream => return error.PortAudioCanNotReadFromACallbackStream,
            c.paCanNotWriteToACallbackStream => return error.PortAudioCanNotWriteToACallbackStream,
            c.paCanNotReadFromAnOutputOnlyStream => return error.PortAudioCanNotReadFromAnOutputOnlyStream,
            c.paCanNotWriteToAnInputOnlyStream => return error.PortAudioCanNotWriteToAnInputOnlyStream,
            c.paIncompatibleStreamHostApi => return error.PortAudioIncompatibleStreamHostApi,
            c.paBadBufferPtr => return error.PortAudioBadBufferPtr,
            c.paCanNotInitializeRecursively => return error.PortAudioCanNotInitializeRecursively,
            else => panic("error not recognized", .{}),
        }
    }
}

pub const android = struct {
    extern fn PaAndroid_SetJavaContext(vm: *anyopaque, context: ?*anyopaque) void;
    pub fn setJavaContext(vm: *anyopaque, context: ?*anyopaque) void {
        if (comptime !builtin.abi.isAndroid()) return;
        PaAndroid_SetJavaContext(vm, context);
    }
};

pub fn init() !PortAudio {
    try errify(c.Pa_Initialize());
    return PortAudio{};
}
pub fn deinit(_: *PortAudio) void {
    errify_print(c.Pa_Terminate()) catch {};
}
pub fn get_host_api_count(_: *PortAudio) usize {
    const res = c.Pa_GetHostApiCount();
    errify_print(res) catch return 0;
    return @intCast(res);
}
pub fn get_host_api_info(self: *PortAudio, host_idx: usize) *const c.struct_PaHostApiInfo {
    assert(host_idx < self.get_host_api_count());
    const res = c.Pa_GetHostApiInfo(@intCast(host_idx));
    return res.?;
}

pub fn get_device_count(_: *PortAudio) usize {
    const res = c.Pa_GetDeviceCount();
    errify_print(res) catch return 0;
    return @intCast(res);
}

pub fn get_device_info(self: *PortAudio, device_idx: usize) *const c.struct_PaDeviceInfo {
    assert(device_idx < self.get_device_count());
    const ret: ?*const c.struct_PaDeviceInfo = c.Pa_GetDeviceInfo(@intCast(device_idx)).?;
    if (ret) |r| return r else unreachable;
}
pub fn index_of_default_input_device(_: *PortAudio) !usize {
    const res = c.Pa_GetDefaultInputDevice();
    try errify_print(res);
    return @intCast(res);
}
pub fn index_of_default_output_device(_: *PortAudio) !usize {
    const res = c.Pa_GetDefaultOutputDevice();
    try errify_print(res);
    return @intCast(res);
}

pub const Stream = struct {
    const Config = struct {
        input_params: ?c.PaStreamParameters,
        output_params: ?c.PaStreamParameters,
        srate: f64,
        frames: u64,
        flags: c.PaStreamFlags,
    };
    raw: *c.PaStream = undefined,
    pub fn init(
        cfg: Stream.Config,
        callback: c.PaStreamCallback,
        user_data: ?*anyopaque,
    ) !Stream {
        var ptr: ?*c.PaStream = null;
        const err = c.Pa_OpenStream(
            &ptr,
            if (cfg.input_params) |*p| p else null,
            if (cfg.output_params) |*p| p else null,
            cfg.srate,
            @intCast(cfg.frames),
            cfg.flags,
            callback,
            user_data,
        );
        try errify_print(err);
        return Stream{
            .raw = ptr.?,
        };
    }
    pub fn start(self: *Stream) !void {
        const err = c.Pa_StartStream(self.raw);
        try errify_print(err);
    }
    pub fn stop(self: *Stream) !void {
        const err = c.Pa_StopStream(self.raw);
        try errify_print(err);
    }
    pub fn abort(self: *Stream) !void {
        const err = c.Pa_AbortStream(self.raw);
        try errify_print(err);
    }
    pub fn close(self: *Stream) !void {
        const err = c.Pa_CloseStream(self.raw);
        try errify_print(err);
    }
    pub fn is_active(self: *Stream) !bool {
        const err = c.Pa_IsStreamActive(self.raw);
        try errify_print(err);
        return err == 1;
    }
    pub fn is_stopped(self: *Stream) !bool {
        const err = c.Pa_IsStreamStopped(self.raw);
        try errify_print(err);
        return err == 1;
    }
    pub fn get_info(self: *Stream) !*const c.struct_PaStreamInfo {
        const ret: ?*const c.struct_PaStreamInfo = c.Pa_GetStreamInfo(self.raw);
        if (ret) |r| return r else return error.PortAudioNullPointer;
    }
    pub fn get_time(self: *Stream) f64 {
        return c.Pa_GetStreamTime(self.raw);
    }
    pub fn get_cpu_load(self: *Stream) f64 {
        return c.Pa_GetStreamCpuLoad(self.raw);
    }
};
fn get_sample_format_type(self: c.PaSampleFormat) type {
    return switch (self) {
        c.paInt8 => i8,
        c.paInt16 => i16,
        c.paInt24 => i24,
        c.paInt32 => i32,
        c.paFloat32 => f32,
    };
}

pub const TStreamF32 = struct {
    const UserData = struct {
        t: *anyopaque,
        t_callback: *const fn (*anyopaque, []const f32, []f32, usize) void,
        in_channels: usize,
        out_channels: usize,
    };
    stream: Stream,
    user_data: UserData,

    fn callback(
        in_ptr: ?*const anyopaque,
        out_ptr: ?*anyopaque,
        cframes: c_ulong,
        _: [*c]const c.PaStreamCallbackTimeInfo,
        _: c.PaStreamCallbackFlags,
        user_data: ?*anyopaque,
    ) callconv(.c) c_int {
        const ud = @as(*UserData, @ptrCast(@alignCast(user_data)));
        const frames: usize = @intCast(cframes);
        const in_slc: []const f32 = if (in_ptr) |p| @as([*]const f32, @ptrCast(@alignCast(p)))[0 .. frames * ud.in_channels] else &.{};
        const out_slice: []f32 = if (out_ptr) |p| @as([*]f32, @ptrCast(@alignCast(p)))[0 .. frames * ud.out_channels] else &.{};
        ud.t_callback(ud.t, in_slc, out_slice, frames);
        return c.paContinue;
    }
    pub fn init(
        self: *@This(),
        t: *anyopaque,
        t_callback: *const fn (*anyopaque, []const f32, []f32, usize) void,
        flags: ?c.PaStreamFlags,
        in_device_idx: usize,
        in_channels: usize,
        out_device_idx: usize,
        out_channels: usize,
        srate: f64,
        frames: usize,
    ) !void {
        self.user_data = UserData{
            .in_channels = in_channels,
            .out_channels = out_channels,
            .t = t,
            .t_callback = t_callback,
        };
        const config: Stream.Config = .{
            .flags = flags orelse c.paNoFlag,
            .frames = frames,
            .input_params = if (in_channels == 0) null else .{
                .channelCount = @intCast(in_channels),
                .sampleFormat = c.paFloat32,
                .suggestedLatency = 0,
                .device = @intCast(in_device_idx),
                .hostApiSpecificStreamInfo = null,
            },
            .output_params = if (out_channels == 0) null else .{
                .channelCount = @intCast(out_channels),
                .sampleFormat = c.paFloat32,
                .suggestedLatency = 0,
                .device = @intCast(out_device_idx),
                .hostApiSpecificStreamInfo = null,
            },
            .srate = srate,
        };
        self.stream = try Stream.init(
            config,
            callback,
            &self.user_data,
        );
    }
};

fn print_device_info(
    pa: *PortAudio,
    dv_idx: usize,
) void {
    const info = pa.get_device_info(dv_idx);
    const fmt =
        \\ device {s}, idx: {} info
        \\ def input latency, high: {} low: {}
        \\ def output latency, high: {} low: {}
        \\ def srate: {}
        \\ in channels: {} out channels {}
        \\ host api: {s}
        \\
    ;
    const host_api = pa.get_host_api_info(@intCast(info.hostApi));

    std.log.warn(fmt, .{
        info.name,
        dv_idx,
        info.defaultHighInputLatency,
        info.defaultLowInputLatency,
        info.defaultHighOutputLatency,
        info.defaultLowOutputLatency,
        info.defaultSampleRate,
        info.maxInputChannels,
        info.maxOutputChannels,
        host_api.name,
    });
}
pub const CallbackTimeInfo = c.PaStreamCallbackTimeInfo;
pub const CallbackFlags = c.PaStreamCallbackFlags;
