#ifndef KERNEL_MQUEUE_H
#define KERNEL_MQUEUE_H

#include <stdint.h>

typedef struct vfs_node vfs_node_t;

#define MQ_MAX_QUEUES 16
#define MQ_MAX_MESSAGES 32
#define MQ_MAX_MESSAGE_SIZE 256

#define MQ_E_FULL  (-2)
#define MQ_E_EMPTY (-3)
#define MQ_E_TIMEOUT (-4)

typedef struct {
    int used;
    int id;
    unsigned int max_messages;
    unsigned int msg_size;
    unsigned int count;
} mqueue_info_t;

void mqueue_init(void);
int mqueue_create(unsigned int max_messages, unsigned int msg_size);
int mqueue_send(int queue_id, const void* message, unsigned int size);
int mqueue_receive(int queue_id, void* buffer, unsigned int buffer_size, unsigned int* received_size_out);
int mqueue_unlink(int queue_id);
int mqueue_list(mqueue_info_t* out, int max_queues);
int mqueue_read_event_id(int queue_id);
int mqueue_write_event_id(int queue_id);
int mqueue_open_fd(int queue_id, unsigned int open_flags);
int mqueue_node_is_queue(const vfs_node_t* node);
int mqueue_node_is_valid(const vfs_node_t* node);
int mqueue_fd_read_ready(const vfs_node_t* node);
int mqueue_fd_write_ready(const vfs_node_t* node);

#endif