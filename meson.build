project('cogl-tests', 'c')

cogl_dep = dependency('cogl-1.0')
gio_dep = dependency('gio-2.0')
gtk_dep = dependency('gtk+-3.0')
gdk_dep = dependency('gdk-pixbuf-2.0')
gdk_dep = dependency('gdk-pixbuf-2.0')

add_global_arguments('-DCOGL_ENABLE_EXPERIMENTAL_API', language : 'c')

executable('image-resize', 'image-resize.c', dependencies : [cogl_dep, gio_dep, gtk_dep, gdk_dep])
