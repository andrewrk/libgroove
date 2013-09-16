typedef void GrooveQueue;

// you may pass NULL for cleanup if you do not need a cleanup function.
GrooveQueue * groove_queue_create(int (*cleanup)(void *obj));

void groove_queue_flush(GrooveQueue *queue);

void groove_queue_destroy(GrooveQueue *queue);

void groove_queue_abort(GrooveQueue *queue);

int groove_queue_put(GrooveQueue *queue, void *obj);

int groove_queue_get(GrooveQueue *queue, void **obj_ptr, int block);

// you may pass this if your cleanup function is simply to free the memory
int groove_queue_cleanup_free(void *obj);
