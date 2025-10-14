systemd = dependency('systemd')
systemd_system_unit_dir = systemd.get_variable(
    'systemd_system_unit_dir',
    pkgconfig_define: ['prefix', get_option('prefix')],
)

unit_files = [
    ['adc', 'xyz.openbmc_project.adcsensor.service'],
    ['cable-monitor', 'xyz.openbmc_project.cablemonitor.service'],
    ['exit-air', 'xyz.openbmc_project.exitairsensor.service'],
    ['synth', 'xyz.openbmc_project.synthesizedsensor.service'],
    ['external', 'xyz.openbmc_project.externalsensor.service'],
    ['fan', 'xyz.openbmc_project.fansensor.service'],
    ['presence-detect', 'xyz.openbmc_project.presence-detect.service'],
    ['hwmon-temp', 'xyz.openbmc_project.hwmontempsensor.service'],
    ['plx-temp', 'xyz.openbmc_project.plxtempsensor.service'],
    ['mctpheartbeat', 'xyz.openbmc_project.mctpheartbeatapp.service'],
    ['intel-cpu', 'xyz.openbmc_project.intelcpusensor.service'],
    ['intrusion', 'xyz.openbmc_project.intrusionsensor.service'],
    ['ipmb', 'xyz.openbmc_project.ipmbsensor.service'],
    ['leakdetector', 'xyz.openbmc_project.leakdetector.service'],
    ['mctp', 'xyz.openbmc_project.mctpreactor.service'],
    ['mcu', 'xyz.openbmc_project.mcutempsensor.service'],
    ['nvidia-gpu', 'xyz.openbmc_project.nvidiagpusensor.service'],
    ['nvme', 'xyz.openbmc_project.nvmesensor.service'],
    ['nvme-mi', 'xyz.openbmc_project.nvmemisensor.service'],
    ['psu', 'xyz.openbmc_project.psusensor.service'],
    ['external', 'xyz.openbmc_project.externalsensor.service'],
    ['procstatus', 'xyz.openbmc_project.processorstatus.service'],
    ['ipmbstatus', 'xyz.openbmc_project.ipmbstatus.service'],
    ['nvmeu2', 'xyz.openbmc_project.nvmestatus.service'],
    ['system', 'xyz.openbmc_project.systemsensor.service'],
    ['psuredundancy', 'xyz.openbmc_project.psuredundancy.service'],
    ['boot-progress', 'xyz.openbmc_project.bootprogress.service'],
    ['satellite', 'xyz.openbmc_project.satellitesensor.service'],
    ['leak-detect', 'xyz.openbmc_project.leakdetectsensor.service'],
    [
        'discrete-leak-detect',
        'xyz.openbmc_project.discreteleakdetectsensor.service',
    ],
    ['write-protect', 'xyz.openbmc_project.writeprotectsensor.service'],
    ['smbpbi', 'xyz.openbmc_project.smbpbisensor.service'],
]

fs = import('fs')
foreach tuple : unit_files
    if get_option(tuple[0]).allowed()
        fs.copyfile(
            tuple[1],
            install: true,
            install_dir: systemd_system_unit_dir,
        )
    endif
endforeach
