#ifndef MESH_PLUGIN_H
#define MESH_PLUGIN_H

/* Lock file in the user's home directory. A dsync session holds it
   exclusively for its whole lifetime, and local saves hold it shared while
   committing, so that no local mail gets a UID while dsync is importing
   mails for the same user. */
#define MESH_LOCK_FILENAME ".dovecot-mesh.lock"

extern const char *mesh_plugin_dependencies[];

void mesh_plugin_init(struct module *module);
void mesh_plugin_deinit(void);

#endif
