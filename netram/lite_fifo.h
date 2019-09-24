#ifndef LITE_FIFO_H
#define LITE_FIFO_H

#ifdef __cplusplus
extern "C" {
#endif
 
#include <stdint.h>

/* struct for holding FIFO metadata */
typedef struct {
    uint8_t obj_sz;
    size_t head;
    size_t tail;
    uint8_t * bufptr;
    size_t byte_count;
    size_t buffer_sz;
} fifo_t;

/* Written to handle arbitrary object sizes */
/* basic FIFO functions, nothing special */
void fifo_init(fifo_t * fifo, void * obj_array, size_t array_sz, uint8_t _obj_sz);
uint8_t fifo_enqueue(fifo_t * fifo, void * obj);
uint8_t fifo_dequeue(fifo_t * fifo, void * obj);
size_t fifo_available(fifo_t * fifo);
uint8_t fifo_full(fifo_t * fifo);

#ifdef __cplusplus
}
#endif

#endif
