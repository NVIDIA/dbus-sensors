add_languages('c', required: true)

gpuserver_lib = static_library(
    'gpuserver',
    ['mctp.c',
     'base.c',
     'instance-id.c',
     'gpuserver.c',
     'gpuserver_mctp_discovery.c'],
    install: true,
    install_dir: get_option('libdir')
)

gpuserver_lib_dep = declare_dependency(
    link_with: gpuserver_lib,
    include_directories: include_directories('.')
)

# Install headers
install_headers('gpuserver.h')
install_headers('gpuserver_mctp_discovery.h')

install_subdir(
    'instance-db',
    install_mode: 'r--r--r--',
    install_dir: get_option('datadir') / meson.project_name(),
)

conf_data = configuration_data()
conf_data.set(
    'INSTANCE_ID_EXPIRATION_INTERVAL',
    get_option('instance-id-expiration-interval'),
)
conf_data.set(
    'NUMBER_OF_REQUEST_RETRIES',
    get_option('number-of-request-retries'),
)
conf_data.set('RESPONSE_TIME_OUT', get_option('response-time-out'))
if get_option('enable-in-kernel-mctp').enabled()
    conf_data.set('MCTP_IN_KERNEL', 0)
endif
configure_file(output: 'config.h', configuration: conf_data)
