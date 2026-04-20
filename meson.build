project('gst-nori', 'cpp',
  version : '1.0.1',
  meson_version : '>= 0.54',
  default_options : ['cpp_std=c++14', 'warning_level=1'])

gst_dep      = dependency('gstreamer-1.0',      version : '>= 1.16')
gst_base_dep = dependency('gstreamer-base-1.0',  version : '>= 1.16')
gst_video_dep = dependency('gstreamer-video-1.0', version : '>= 1.16')

nori_sdk_inc = include_directories('sdk/include')

# Auto-detect host architecture and select matching SDK library
host_cpu = host_machine.cpu_family()
if host_cpu == 'x86_64'
  nori_arch = 'x64'
elif host_cpu == 'x86'
  nori_arch = 'x86'
elif host_cpu == 'aarch64'
  nori_arch = 'arm64'
elif host_cpu == 'arm'
  nori_arch = 'arm32'
else
  error('Unsupported CPU architecture: ' + host_cpu)
endif

nori_lib_dir = meson.current_source_dir() / 'sdk' / 'lib' / nori_arch
nori_lib = meson.get_compiler('cpp').find_library('Nori_Xvision_Std',
  dirs : [nori_lib_dir])

# Where GStreamer looks for plugins
gst_plugins_dir = get_option('prefix') / get_option('libdir') / 'gstreamer-1.0'

shared_library('gstnori',
  'src/gstnorisrc.cpp',
  'src/gstnori.cpp',
  include_directories : nori_sdk_inc,
  dependencies : [gst_dep, gst_base_dep, gst_video_dep, nori_lib],
  install : true,
  install_dir : gst_plugins_dir,
  build_rpath : nori_lib_dir,
  cpp_args : ['-DGST_USE_UNSTABLE_API',
              '-DPACKAGE="gst-nori"',
              '-DVERSION="1.0.1"'],
)
