/* Copyright (c) Dovecot authors, see top-level COPYING file */

#include "lib.h"
#include "mesh-ui.h"

const struct mesh_ui_file *mesh_ui_file_find(const char *name)
{
	const struct mesh_ui_file *file;

	for (file = mesh_ui_files; file->name != NULL; file++) {
		if (strcmp(file->name, name) == 0)
			return file;
	}
	return NULL;
}
