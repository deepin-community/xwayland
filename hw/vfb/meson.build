srcs = [
    'InitInput.c',
    'InitOutput.c',
    '../../mi/miinitext.c',
    '../../mi/miinitext.h',
]

xvfb_server = executable(
    'Xvfb',
    srcs,
    include_directories: inc,
    dependencies: common_dep,
    link_with: [
        libxserver_main,
        libxserver_fb,
        libxserver,
        libxserver_xkb_stubs,
        libxserver_xi_stubs,
        libxserver_glx,
        libglxvnd,
    ],
    install: false,
)
