unit_files = [
    'gpuserver-mctp-reactor.service',
    'gpuserverd.service',
]

foreach unit : unit_files

    configure_file(
        input: unit,
        output: unit,
        copy: true,
        install: true,
        install_dir: systemd_system_unit_dir,
    )

endforeach
