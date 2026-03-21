#ifndef FS_H
#define FS_H

void fs_init(void);
int fs_count(void);
const char* fs_name_at(int index);
unsigned int fs_size(const char* name);
int fs_exists(const char* name);
int fs_read(const char* name, char* buffer, unsigned int buffer_size);
int fs_read_bytes(const char* name, unsigned char* buffer, unsigned int buffer_size);
int fs_write(const char* name, const unsigned char* data, unsigned int size, int append);

#endif
