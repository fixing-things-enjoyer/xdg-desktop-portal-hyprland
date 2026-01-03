globber = run_command('find', '.', '-name', '*.cpp', check: true)
src = globber.stdout().strip().split('\n')

deps = [
  client_protos,
  dependency('gbm'),
  dependency('hyprlang'),
  dependency('hyprutils'),
  dependency('libdrm'),
  dependency('libpipewire-0.3'),
  dependency('sdbus-c++'),
  dependency('threads'),
  dependency('wayland-client'),
]

cpp_args = ['-DXDPH_VERSION="' + meson.project_version() + '"']
shader_inc = []

gpu_rotation_opt = get_option('gpu_screen_rotation')
vulkan_dep = dependency('vulkan', required: gpu_rotation_opt)

if vulkan_dep.found()
  glslc = find_program('glslc', 'glslangValidator', required: gpu_rotation_opt)
  
  if glslc.found()
    deps += vulkan_dep
    cpp_args += '-DHAS_VULKAN_ROTATION=1'
    
    rotate_shader_spv = custom_target('rotate_shader_spv',
      input: 'shaders/rotate.comp',
      output: 'rotate.comp.spv',
      command: [glslc, '@INPUT@', '-o', '@OUTPUT@']
    )
    
    xxd = find_program('xxd', required: false)
    if xxd.found()
      rotate_shader_inc = custom_target('rotate_shader_inc',
        input: rotate_shader_spv,
        output: 'rotate_shader.inc',
        capture: true,
        command: [
          'sh', '-c',
          'xxd -i < "$1" | grep "0x"',
          'dummy',
          '@INPUT@'
        ]
      )
      shader_inc += rotate_shader_inc
    else
      python = find_program('python3')
      rotate_shader_inc = custom_target('rotate_shader_inc',
        input: rotate_shader_spv,
        output: 'rotate_shader.inc',
        command: [
          python, '-c',
          '''
import sys
with open(sys.argv[1], 'rb') as f:
    data = f.read()
with open(sys.argv[2], 'w') as f:
    for i, b in enumerate(data):
        if i % 12 == 0:
            f.write('\n  ')
        f.write(f'0x{b:02x},')
    f.write('\n')
''',
          '@INPUT@', '@OUTPUT@'
        ]
      )
      shader_inc += rotate_shader_inc
    endif
    
    message('GPU screen rotation enabled (Vulkan and shader compiler found)')
  else
    message('GPU screen rotation disabled (shader compiler not found)')
  endif
else
  message('GPU screen rotation disabled (Vulkan not found)')
endif

executable('xdg-desktop-portal-hyprland',
  [src, shader_inc],
  dependencies: deps,
  cpp_args: cpp_args,
  include_directories: inc,
  install: true,
  install_dir: get_option('libexecdir')
)
