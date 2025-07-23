executable(
    'mctpreactor',
    'MCTPReactorMain.cpp',
    'MCTPReactor.cpp',
    'MCTPEndpoint.cpp',
    dependencies: [ default_deps, utils_dep ],
    install: true
)

if get_option('mctpheartbeat').enabled()
    executable(
        'mctpheartbeat',
        'MCTPHeartBeatApp.cpp',
        dependencies: [ default_deps, utils_dep ],
        install: true
    )
endif