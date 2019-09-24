#ifndef NETRAM_H_
#define NETRAM_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

/* comment out to disable debug messages */
#define DBG_ENABLE

#if defined(DBG_ENABLE)
    #define DEBUG_PRINT(...)            printf(__VA_ARGS__)
    #define DEBUG_PRINTLN(fmt, ...)     printf(fmt"\n", ##__VA_ARGS__)
#else
    #define DEBUG_PRINT(...)
    #define DEBUG_PRINTLN(...)
#endif

/* uncomment so that a static buffer is (re)used for MPI data transfers,
   necessary for Infiniband */
#define NETRAM_REUSE_BUFFER

/* minimum allocatable unit; default 1 MB */
#define NETRAM_UNIT_SZ                  (1 * 1024UL * 1024UL)
/* maximum mappable units, equivalent to 500 GB with defaults */
#define NETRAM_MAX_MAPPABLE_UNITS       (500 * 1024UL)
/* maximum we can hold actively before swapping; 3 GB default*/
#define NETRAM_MAX_ACTIVE_UNITS         (3UL * 1024)
#define NETRAM_MAX_ACTIVE_MEM           (NETRAM_MAX_ACTIVE_UNITS * NETRAM_UNIT_SZ)

/* maximum we can swap in/out; default 64 MB, increase as you like */
#define NETRAM_MAX_TRANSFER_BYTES       (64 * NETRAM_UNIT_SZ)

/* msg tags for indicating MPI R/W operation to slaves */
#define NETRAM_MSG_WRITE    (1)
#define NETRAM_MSG_READ     (2)

/* Holds metadata of user-allocated blocks,
   overhead of 24 bytes; potentially about 12 MB for a 500 GB VMA */
typedef struct block_meta {
    size_t size;
    struct block_meta * next;
    
    bool free;
    bool ever_swapped;
    uint8_t padding[sizeof(void *) - 2];
} block_meta_t;

/* used to hold records of units loaded into RAM */
typedef struct {
    block_meta_t * block;
    void * unit;
    size_t size;
} active_block_record_t;

/* used to hold slave VMA details */
typedef struct {
    uint8_t rank;
    void * addr;
    size_t size;
} netram_slaves_t;

/* used to send messages between nodes */
typedef struct {
    uint64_t addr;
    uint64_t size;
} netram_msg_t;

bool netram_init(size_t max_units);
void * netram_malloc(size_t size);
void netram_free(void * ptr);
void netram_end(void);

/* dump some debug info about data blocks */
void netram_dump(void);

#ifdef __cplusplus
}
#endif

#endif