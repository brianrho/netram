/* Written by Brian Ejike (2019)
 * Distributed under the MIT License */
 
#include <string.h>
#include "lite_fifo.h"

/* Initialize a generic FIFO with an array of any data type,
 * the number of elements in the array, and the size in bytes of each element
 */
void fifo_init(fifo_t * fifo, void * obj_array, size_t array_sz, uint8_t _obj_sz)
{
    fifo->head = 0;
    fifo->tail = 0;
    fifo->byte_count = 0;
    fifo->bufptr = obj_array;
    fifo->buffer_sz = array_sz * _obj_sz;
    fifo->obj_sz = _obj_sz;
}

uint8_t fifo_dequeue(fifo_t * fifo, void * obj) {
    if (fifo->byte_count == 0)
        return 0;
    
    uint8_t * ptr = (uint8_t *)obj;
    memcpy(ptr, &fifo->bufptr[fifo->tail], fifo->obj_sz);
    fifo->tail += fifo->obj_sz;
    if (fifo->tail == fifo->buffer_sz)
        fifo->tail = 0;
    fifo->byte_count -= fifo->obj_sz;
    return 1;
}

uint8_t fifo_enqueue(fifo_t * fifo, void * obj) {
    if (fifo->byte_count == fifo->buffer_sz)
        return 0;
    
    uint8_t * ptr = (uint8_t *)obj;
    memcpy(&fifo->bufptr[fifo->head], ptr, fifo->obj_sz);
    fifo->head += fifo->obj_sz;
    if (fifo->head == fifo->buffer_sz)
        fifo->head = 0;
    fifo->byte_count += fifo->obj_sz;
    return 1;
}

size_t fifo_available(fifo_t * fifo) {
    return fifo->byte_count / fifo->obj_sz;
}

uint8_t fifo_full(fifo_t * fifo) {
    return (fifo->byte_count == fifo->buffer_sz);
}
