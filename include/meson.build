libfuse_headers = [ 'fuse.h', 'fuse_common.h', 'fuse_lowlevel.h',
	            'fuse_opt.h', 'fuse_log.h' ]

if host_machine.system() != 'darwin'
   libfuse_headers += [ 'cuse_lowlevel.h' ]
endif

install_headers(libfuse_headers, subdir: 'fuse3')
