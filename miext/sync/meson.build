srcs_miext_sync = [
    'misync.c',
    'misyncfd.c',
]

hdrs_miext_sync = [
    'misync.h',
    'misyncfd.h',
    'misyncshm.h',
    'misyncstr.h',
]

if build_dri3 or build_xwayland
    srcs_miext_sync += 'misyncshm.c'
endif

libxserver_miext_sync = static_library('libxserver_miext_sync',
    srcs_miext_sync,
    include_directories: inc,
    dependencies: [
        common_dep,
        xshmfence_dep,
    ],
)
