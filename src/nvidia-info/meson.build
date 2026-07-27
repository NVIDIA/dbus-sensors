# Schema / derivation sources. Carved out as a separate list so the
# libFuzzer harness in tests/ can link only the JSON parsing and
# validation code without dragging in NvidiaInfo.cpp's asio + D-Bus
# event loop.
nvidia_info_schema_sources = files(
    'NvidiaInfoCpu.cpp',
    'NvidiaInfoDimm.cpp',
    'NvidiaInfoPcie.cpp',
    'NvidiaInfoSchema.cpp',
    'NvidiaInfoTpm.cpp',
)

# Library sources: schema + the asio/sd-bus glue. Linked into the daemon
# executable and the gtest binary; excludes NvidiaInfoMain.cpp so gtest
# can supply its own main.
nvidia_info_lib_sources = nvidia_info_schema_sources + files(
    'NvidiaInfo.cpp',
    'NvidiaInfoPersistence.cpp',
)

nvidia_info_sources = nvidia_info_lib_sources + files('NvidiaInfoMain.cpp')

nvidia_info_include_dir = include_directories('.')
sensor_include_dir = include_directories('../..')

# Embed the per-terminus JSON schema directly into the binary so runtime
# validation does not depend on any on-disk schema location.
fs = import('fs')
nvidia_info_schema_embed = configure_file(
    input: 'NvidiaInfoSchemaEmbed.hpp.in',
    output: 'NvidiaInfoSchemaEmbed.hpp',
    configuration: {'SCHEMA_JSON': fs.read('schema.json')},
)

nlohmann_json_schema_validator_dep = dependency(
    'nlohmann_json_schema_validator',
    include_type: 'system',
)

executable(
    'nvidiainfo',
    nvidia_info_sources,
    nvidia_info_schema_embed,
    cpp_args: [
        '-DJSON_IMPLICIT_CONVERSIONS=0',
        '-DNVIDIA_INFO_SOCKETS_PER_MODULE=' + get_option(
            'nvidia-info-sockets-per-module',
        ).to_string(),
        '-DNVIDIA_INFO_HGX_PREFIX="' + get_option('nvidia-info-hgx-prefix') + '"',
    ],
    implicit_include_directories: false,
    include_directories: [nvidia_info_include_dir, sensor_include_dir],
    dependencies: [
        default_deps,
        nlohmann_json_dep,
        nlohmann_json_schema_validator_dep,
    ],
    install: true,
    install_dir: '/usr/libexec/dbus-sensors',
)

subdir('tests')
