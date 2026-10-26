#ifndef MESH_UI_H
#define MESH_UI_H

/* The web UI files, embedded into the binary at build time from ui/ by
   mesh-ui-gen.pl, so that no install path needs configuring. */
struct mesh_ui_file {
	const char *name;
	const char *content_type;
	const unsigned char *data;
	size_t size;
};

extern const struct mesh_ui_file mesh_ui_files[];

const struct mesh_ui_file *mesh_ui_file_find(const char *name);

#endif
