src_inc = ['..']
if not meson.get_compiler('cpp').has_header('linux/peci-ioctl.h')
    src_inc += ['../../include']
endif

# MCTP/PECI-over-MCTP sources, compiled directly into the intel-cpu targets to
# keep them independent of the struct-based nvidia-gpu OcpMctpVdm/MctpRequester.
mctp_peci_sources = files(
    'MctpAsioEndpoint.cpp',
    'MctpRequester.cpp',
    'OcpMctpVdm.cpp',
    'PeciMctp.cpp',
)

# Legacy /dev/peci CPU sensor, gated on the existing intel-cpu package config.
if get_option('intel-cpu').allowed()
    peci_dep = dependency('libpeci', required: true)
    executable(
        'intelcpusensor',
        'IntelCPUSensorMain.cpp',
        'IntelCPUSensor.cpp',
        'IntelCPUMctpSensor.cpp',
        'IntelCPUMctpTempSensor.cpp',
        mctp_peci_sources,
        dependencies: [
            default_deps,
            gpiodcxx,
            thresholds_dep,
            utils_dep,
            peci_dep,
            boost,
            phosphor_logging_dep,
            uring,
        ],
        include_directories: src_inc,
        install: true,
        install_dir: '/usr/libexec/dbus-sensors',
    )
endif

# Discovery daemon for Intel CPUs reached over MCTP (PECI-over-MCTP). Does not
# use libpeci; identifies CPUs via entity-manager + mctpd + a PECI ping. Gated
# on its own intel-cpu-mctp package config, independent of the legacy sensor.
if get_option('intel-cpu-mctp').allowed()
    executable(
        'intelcpumctpsensor',
        'IntelCPUMctpSensorMain.cpp',
        'IntelCPUMctpSensor.cpp',
        'IntelCPUMctpTempSensor.cpp',
        mctp_peci_sources,
        dependencies: [
            default_deps,
            gpiodcxx,
            thresholds_dep,
            utils_dep,
            boost,
            phosphor_logging_dep,
            uring,
        ],
        include_directories: src_inc,
        install: true,
        install_dir: '/usr/libexec/dbus-sensors',
    )
endif
