#include "mqueue.h"
#include "heap.h"
#include "string.h"

typedef struct {
    int used;
    int id;
    unsigned int max_messages;
    unsigned int msg_size;
    unsigned int head;
    unsigned int tail;
    unsigned int count;
    unsigned int lengths[MQ_MAX_MESSAGES];
    unsigned char* storage;
} mqueue_t;

static mqueue_t queues[MQ_MAX_QUEUES];
static int next_queue_id = 1;

static void mqueue_zero_memory(unsigned char* buffer, unsigned int size) {
    for (unsigned int i = 0; i < size; i++) {
        buffer[i] = 0U;
    }
}

static void mqueue_copy_memory(unsigned char* dst, const unsigned char* src, unsigned int size) {
    for (unsigned int i = 0; i < size; i++) {
        dst[i] = src[i];
    }
}

static mqueue_t* mqueue_find(int queue_id) {
    for (int i = 0; i < MQ_MAX_QUEUES; i++) {
        if (queues[i].used && queues[i].id == queue_id) {
            return &queues[i];
        }
    }

    return 0;
}

void mqueue_init(void) {
    for (int i = 0; i < MQ_MAX_QUEUES; i++) {
        queues[i].used = 0;
        queues[i].id = 0;
        queues[i].max_messages = 0U;
        queues[i].msg_size = 0U;
        queues[i].head = 0U;
        queues[i].tail = 0U;
        queues[i].count = 0U;
        queues[i].storage = 0;
        for (int j = 0; j < MQ_MAX_MESSAGES; j++) {
            queues[i].lengths[j] = 0U;
        }
    }

    next_queue_id = 1;
}

int mqueue_create(unsigned int max_messages, unsigned int msg_size) {
    unsigned int bytes;
    unsigned char* storage;
    int slot = -1;

    if (max_messages == 0U || max_messages > MQ_MAX_MESSAGES ||
        msg_size == 0U || msg_size > MQ_MAX_MESSAGE_SIZE) {
        return -1;
    }

    bytes = max_messages * msg_size;
    if (bytes / msg_size != max_messages) {
        return -1;
    }

    for (int i = 0; i < MQ_MAX_QUEUES; i++) {
        if (!queues[i].used) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        return -1;
    }

    storage = (unsigned char*)kmalloc(bytes);
    if (storage == 0) {
        return -1;
    }

    mqueue_zero_memory(storage, bytes);

    queues[slot].used = 1;
    queues[slot].id = next_queue_id++;
    queues[slot].max_messages = max_messages;
    queues[slot].msg_size = msg_size;
    queues[slot].head = 0U;
    queues[slot].tail = 0U;
    queues[slot].count = 0U;
    queues[slot].storage = storage;
    for (unsigned int i = 0; i < max_messages; i++) {
        queues[slot].lengths[i] = 0U;
    }

    return queues[slot].id;
}

int mqueue_send(int queue_id, const void* message, unsigned int size) {
    mqueue_t* queue;
    unsigned char* slot;

    if (message == 0 || size == 0U) {
        return -1;
    }

    queue = mqueue_find(queue_id);
    if (queue == 0 || !queue->used || queue->storage == 0) {
        return -1;
    }

    if (size > queue->msg_size) {
        return -1;
    }

    if (queue->count >= queue->max_messages) {
        return MQ_E_FULL;
    }

    slot = queue->storage + (queue->head * queue->msg_size);
    mqueue_zero_memory(slot, queue->msg_size);
    mqueue_copy_memory(slot, (const unsigned char*)message, size);
    queue->lengths[queue->head] = size;
    queue->head = (queue->head + 1U) % queue->max_messages;
    queue->count++;
    return 0;
}

int mqueue_receive(int queue_id, void* buffer, unsigned int buffer_size, unsigned int* received_size_out) {
    mqueue_t* queue;
    unsigned int size;
    unsigned char* slot;

    if (buffer == 0) {
        return -1;
    }

    queue = mqueue_find(queue_id);
    if (queue == 0 || !queue->used || queue->storage == 0) {
        return -1;
    }

    if (queue->count == 0U) {
        return MQ_E_EMPTY;
    }

    size = queue->lengths[queue->tail];
    if (buffer_size < size) {
        return -1;
    }

    slot = queue->storage + (queue->tail * queue->msg_size);
    mqueue_copy_memory((unsigned char*)buffer, slot, size);
    queue->lengths[queue->tail] = 0U;
    queue->tail = (queue->tail + 1U) % queue->max_messages;
    queue->count--;

    if (received_size_out != 0) {
        *received_size_out = size;
    }

    return 0;
}

int mqueue_unlink(int queue_id) {
    mqueue_t* queue = mqueue_find(queue_id);

    if (queue == 0) {
        return -1;
    }

    if (queue->storage != 0) {
        kfree(queue->storage);
    }

    queue->used = 0;
    queue->id = 0;
    queue->max_messages = 0U;
    queue->msg_size = 0U;
    queue->head = 0U;
    queue->tail = 0U;
    queue->count = 0U;
    queue->storage = 0;
    for (int i = 0; i < MQ_MAX_MESSAGES; i++) {
        queue->lengths[i] = 0U;
    }

    return 0;
}

int mqueue_list(mqueue_info_t* out, int max_queues) {
    int count = 0;

    if (out == 0 || max_queues <= 0) {
        return 0;
    }

    for (int i = 0; i < MQ_MAX_QUEUES && count < max_queues; i++) {
        if (!queues[i].used) {
            continue;
        }

        out[count].used = 1;
        out[count].id = queues[i].id;
        out[count].max_messages = queues[i].max_messages;
        out[count].msg_size = queues[i].msg_size;
        out[count].count = queues[i].count;
        count++;
    }

    return count;
}