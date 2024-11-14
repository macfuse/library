/*
 * Copyright (c) 2006-2008 Amit Singh/Google Inc.
 * Copyright (c) 2012 Anatol Pomozov
 * Copyright (c) 2011-2024 Benjamin Fleischer
 */

#include "fuse_i.h"
#include "fuse_darwin.h"

#include <dlfcn.h>
#include <errno.h>
#include <libgen.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/types.h>
#include <unistd.h>

#include <CoreFoundation/CoreFoundation.h>

/* Resource paths */

#define EXECUTABLE_PATH "@executable_path/"
#define LOADER_PATH "@loader_path/"

char *fuse_resource_path(const char *path)
{
	char base_path[MAXPATHLEN];
	char *relative_path = NULL;
	char *resource_path;

	if (strncmp(path, EXECUTABLE_PATH, sizeof(EXECUTABLE_PATH) - 1) == 0) {
		int      err = 0;
		uint32_t executable_path_len = MAXPATHLEN;

		/* Path relative to executable */
		err = _NSGetExecutablePath(base_path, &executable_path_len);
		if (err == -1) {
			return NULL;
		}

		relative_path = (char *)path + sizeof(EXECUTABLE_PATH) - 1;
	} else if (strncmp(path, LOADER_PATH, sizeof(LOADER_PATH) - 1) == 0) {
		Dl_info info;

		/* Path relative to loader */
		if (!dladdr(&fuse_resource_path, &info)) {
			return NULL;
		}
		strncpy(base_path, info.dli_fname, sizeof(base_path) - 1);
		base_path[sizeof(base_path) - 1] = '\0';

		relative_path = (char *)path + sizeof(LOADER_PATH) - 1;
	}

	if (relative_path) {
		char  base_path_real[MAXPATHLEN];
		char *base_dir;

		if (!realpath(base_path, base_path_real)) {
			return NULL;
		}

		/* Parent directory of base path */
		base_dir = dirname(base_path_real);
		if (!base_dir) {
			return NULL;
		}

		/* Build resource path */
		asprintf(&resource_path, "%s/%s", base_dir, relative_path);
	} else {
		resource_path = malloc(strlen(path) + 1);
		if (!resource_path) {
			return NULL;
		}
		strcpy(resource_path, path);
	}

	return resource_path;
}
