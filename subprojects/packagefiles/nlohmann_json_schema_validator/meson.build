project(
    'nlohmann_json_schema_validator',
    'cpp',
    version: '2.3.0',
    license: 'MIT',
    default_options: ['cpp_std=c++17', 'warning_level=0', 'werror=false'],
)

nlohmann_json_dep = dependency('nlohmann_json', include_type: 'system')

# Source list mirrors upstream's CMakeLists.txt at tag 2.3.0. Pinned by the
# wrap so the file list does not drift; bump both the wrap revision and
# this list when upgrading.
nlohmann_json_schema_validator_sources = files(
    'src/json-patch.cpp',
    'src/json-schema-draft7.json.cpp',
    'src/json-uri.cpp',
    'src/json-validator.cpp',
    'src/smtp-address-validator.cpp',
    'src/string-format-check.cpp',
)

# Public header is `<nlohmann/json-schema.hpp>`, which lives at
# `nlohmann/json-schema.hpp` from the source root. Marked as system so any
# warnings from the headers do not trip the parent project's -Werror.
nlohmann_json_schema_validator_inc = include_directories('.', is_system: true)

nlohmann_json_schema_validator_lib = static_library(
    'nlohmann_json_schema_validator',
    nlohmann_json_schema_validator_sources,
    include_directories: nlohmann_json_schema_validator_inc,
    dependencies: [nlohmann_json_dep],
    cpp_args: [
        '-Wno-unused-parameter',
        '-Wno-shadow',
        '-Wno-deprecated-declarations',
        '-Wno-pedantic',
    ],
    install: false,
)

nlohmann_json_schema_validator_dep = declare_dependency(
    link_with: nlohmann_json_schema_validator_lib,
    include_directories: nlohmann_json_schema_validator_inc,
    dependencies: [nlohmann_json_dep],
)

meson.override_dependency(
    'nlohmann_json_schema_validator',
    nlohmann_json_schema_validator_dep,
)
