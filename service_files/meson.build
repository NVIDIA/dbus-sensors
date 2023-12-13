unit_files = [
    ['adc', 'xyz.openbmc_project.adcsensor.service'],
    ['intel-cpu', 'xyz.openbmc_project.intelcpusensor.service'],
    ['exit-air', 'xyz.openbmc_project.exitairsensor.service'],
    ['fan', 'xyz.openbmc_project.fansensor.service'],
    ['hwmon-temp', 'xyz.openbmc_project.hwmontempsensor.service'],
    ['plx-temp', 'xyz.openbmc_project.plxtempsensor.service'],
    ['ipmb', 'xyz.openbmc_project.ipmbsensor.service'],
    ['intrusion', 'xyz.openbmc_project.intrusionsensor.service'],
    ['mcu', 'xyz.openbmc_project.mcutempsensor.service'],
    ['nvme', 'xyz.openbmc_project.nvmesensor.service'],
    ['psu', 'xyz.openbmc_project.psusensor.service'],
    ['external', 'xyz.openbmc_project.externalsensor.service'],
    ['procstatus', 'xyz.openbmc_project.processorstatus.service'],
    ['ipmbstatus', 'xyz.openbmc_project.ipmbstatus.service'],
    ['nvmeu2', 'xyz.openbmc_project.nvmestatus.service'],
    ['system', 'xyz.openbmc_project.systemsensor.service'],
    ['gpu', 'xyz.openbmc_project.gpustatus.service'],
    ['psuredundancy', 'xyz.openbmc_project.psuredundancy.service'],

]

foreach tuple : unit_files
    if get_option(tuple[0]).allowed()
        configure_file(
            copy: true,
            input: tuple[1],
            install: true,
            install_dir: systemd_system_unit_dir,
            output: tuple[1],
        )
    endif
endforeach
