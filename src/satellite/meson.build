src_inc = include_directories('..')
include_dir = include_directories('../../include')

executable(
    'satellitesensor',
    'SatelliteSensor.cpp',
    'vHMCShmReader.cpp',
    dependencies: [default_deps, i2c, thresholds_dep, utils_dep],
    implicit_include_directories: false,
    include_directories: [src_inc, include_dir],
    install: true,
)

if get_option('tests').allowed()
    subdir('tests')
endif
