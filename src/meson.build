globber = run_command('find', '.', '-name', '*.cpp', check: true)
src = globber.stdout().strip().split('\n')

executable('xdg-desktop-portal-hyprland',
  [src, rotate_shader_header],
  dependencies: [
    client_protos,
    dependency('gbm'),
    dependency('hyprlang'),
    dependency('hyprutils'),
    dependency('libdrm'),
    dependency('libpipewire-0.3'),
    dependency('sdbus-c++'),
    dependency('threads'),
    dependency('wayland-client'),
    vulkan_dep,
  ],
  include_directories: inc,
  install: true,
  install_dir: get_option('libexecdir')
)
