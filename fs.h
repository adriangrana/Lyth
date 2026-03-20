#ifndef FS_H
#define FS_H

void fs_init(void);
int fs_count(void);
const char* fs_name_at(int index);
unsigned int fs_size(const char* name);
int fs_exists(const char* name);
int fs_read(const char* name, char* buffer, unsigned int buffer_size);

#endif
