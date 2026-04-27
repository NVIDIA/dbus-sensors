cxx = meson.get_compiler('cpp')

# Fuzzing requires libFuzzer + leak sanitizer.  Only clang on Linux ships a
# usable combination today (osx clang has no leak detector).  Under any other
# toolchain (e.g. the gcc-based Yocto build) this block is silently skipped
# so `meson test` behavior is unchanged.
#
# Host requirements:
#   * clang (any modern version) for libFuzzer + ASan + LeakSan.
#   * libsystemd >= 247 -- upstream sdbusplus pulls in sd_event_add_time_relative
#     (added in systemd 247) and the post-2020 sd-bus-vtable.h layout.  Hosts
#     older than this (e.g. Ubuntu 20.04 / Focal, libsystemd 245) cannot build
#     the sdbusplus subproject and therefore cannot link the fuzz target.
is_darwin = host_machine.system().startswith('darwin')

if cxx.get_id() == 'clang' and get_option('fuzz').allowed() and not is_darwin
    fuzz_args = [
        '-DFUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION',
        '-fsanitize=fuzzer,address,leak',
    ]

    # Explicit dependency list (no default_deps): nvidia-info schema sources
    # do not directly use nvidia-tal, but boost asio is pulled in transitively
    # via NvidiaInfoPublisher.hpp -> <sdbusplus/asio/object_server.hpp>, and
    # the project-wide -DBOOST_ASIO_HAS_IO_URING forces boost/asio headers to
    # include <liburing.h>, so liburing's include path must be on the line.
    # phosphor-logging is needed for <phosphor-logging/lg2.hpp> in schema
    # sources.
    fuzz_nvidia_info = executable(
        'fuzz_nvidia_info',
        'fuzz_nvidia_info.cpp',
        nvidia_info_schema_sources,
        nvidia_info_schema_embed,
        cpp_args: fuzz_args + ['-DJSON_IMPLICIT_CONVERSIONS=0'],
        link_args: fuzz_args,
        implicit_include_directories: false,
        include_directories: [nvidia_info_include_dir, sensor_include_dir],
        dependencies: [
            boost,
            nlohmann_json_dep,
            nlohmann_json_schema_validator_dep,
            phosphor_logging_dep,
            sdbusplus,
            uring,
        ],
    )

    # libFuzzer takes the first positional arg as the writable corpus dir
    # and any further positional args as read-only seed dirs.  It does not
    # create the writable dir on its own, so make sure it exists at meson
    # configure time.  Keep the seed corpus committed in `corpus/`; let
    # libFuzzer accumulate findings under the build tree so `meson test`
    # never mutates the source.
    run_command(
        'mkdir',
        '-p',
        meson.current_build_dir() / 'corpus_out',
        check: true,
    )
    test(
        'fuzz_nvidia_info',
        fuzz_nvidia_info,
        args: [
            '-max_total_time=10',
            '-max_len=131072',
            '-error_exitcode=1',
            '-timeout_exitcode=2',
            '-artifact_prefix=' + meson.current_build_dir() + '/crash-',
            meson.current_build_dir() + '/corpus_out',
            meson.current_source_dir() + '/corpus',
        ],
        timeout: 120,
    )
endif
