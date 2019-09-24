/* Written by Brian Ejike (2019)
 * Distributed under the MIT License */
 
#include <unistd.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/mman.h>
#include <stdint.h>
#include <sys/time.h>
#include <sys/resource.h>

#include <mpi.h>
#include <assert.h>
#include <math.h>

#include "lite_fifo.h"
#include "netram.h"

#define HANDLE_EXIT(msg)    \
    do {perror(msg); exit(EXIT_FAILURE);} while (0)

#define BLOCK_META_SIZE     sizeof(block_meta_t)
#define MIN(x, y)           (((x) < (y)) ? (x) : (y))

/* round up and align some size to a multiple of the unit size */ 
#define UNIT_ALIGN(x)       (((x) + NETRAM_UNIT_SZ - 1) & ~(NETRAM_UNIT_SZ - 1))

/* specify type of MPI call for shutting down slaves */
enum {
    SHUTDOWN_WITH_SCATTER,
    SHUTDOWN_WITH_SEND
};

/* vars to let us know how segv happened */
static volatile sig_atomic_t segv_happened = 0;
static void * stack_space = NULL;

/* used to check if we already shut down the slaves */
static bool slaves_already_stopped = false;

/* base addresses for data and metadata */
static void * base_data_addr;
static void * base_meta_addr;

static size_t total_free_data;
static size_t total_free_meta;
static netram_slaves_t * slaves_meta = NULL;

MPI_Datatype MPI_Netram_Type;
static int world_rank = 0;
static int world_size = 0;

/* queue for holding metadata of active blocks recently in physical RAM */
static active_block_record_t _active_buf[NETRAM_MAX_ACTIVE_UNITS];
static fifo_t active_fifo;
static size_t curr_active_sz = 0;

/* internal funcs */
static void * find_free_block(size_t size);
static void swap_out_units(void * addr, size_t size);
static void retrieve_units(void * addr, size_t size);

static size_t distribute_vma(size_t num_units);
static void shut_down_slaves(int how);
static bool map_vma_range(size_t num_units);
static void setup_segv_handler(void);
static void segv_handle(int sig, siginfo_t *info, void *vcontext);

#if defined(NETRAM_REUSE_BUFFER)
static uint8_t mpi_buf[NETRAM_MAX_TRANSFER_BYTES];
#endif

bool netram_init(size_t max_units) {
    int initialized;
    
    int page_sz = sysconf(_SC_PAGE_SIZE);
    if (page_sz == -1) {
        DEBUG_PRINTLN("sysconf failed.");
        return false;
    }
    
    if (NETRAM_UNIT_SZ % page_sz != 0) {
        DEBUG_PRINTLN("Non-integer number of pages in unit");
        return false;
    }
    
    /* start up MPI */
    MPI_Initialized(&initialized);
    if (!initialized) {
        MPI_Init(NULL, NULL);
    }
    MPI_Comm_rank(MPI_COMM_WORLD, &world_rank);
    MPI_Comm_size(MPI_COMM_WORLD, &world_size);
    
    DEBUG_PRINTLN("World size: %d", world_size);
    
    /* create MPI type for msgs */
    MPI_Type_contiguous(2, MPI_UINT64_T, &MPI_Netram_Type);
    MPI_Type_commit(&MPI_Netram_Type);
    
    /* assign default VMA size */
    if (max_units == 0)
        max_units = NETRAM_MAX_MAPPABLE_UNITS;
    
    /* distribute VMA between slaves */
    total_free_data = distribute_vma(max_units);
    
    /* if something went wrong, clean up */
    if (total_free_data == 0) {
        netram_end();
        return false;
    }
    
    /* init the first metadata structure
       with a size spanning the whole VMA, mark it as free */
    block_meta_t * block = base_meta_addr;
    block->size = total_free_data;
    block->next = NULL;
    block->free = true;
    block->ever_swapped = false;
    
    /* init queue for active blocks */
    fifo_init(&active_fifo, _active_buf, NETRAM_MAX_ACTIVE_UNITS, sizeof(active_block_record_t));
    
    DEBUG_PRINTLN("%d: Total VMA size: %lu bytes", world_rank, total_free_data);
    /* setup handler */
    setup_segv_handler();
    return true;
}

/* just used for sorting slave capacities */
static int compare(const void * first, const void * second) {
    return (((netram_msg_t *)first)->size - ((netram_msg_t *)second)->size);
}

/* distribute the entire VMA between the slaves */  
static size_t distribute_vma(size_t num_units) {
    size_t total_needed = num_units * NETRAM_UNIT_SZ;
    
    /* obtain available memory from slaves */
    uint64_t * slaves_space = calloc(world_size, sizeof(uint64_t));
    if (slaves_space == NULL)
        HANDLE_EXIT("malloc failed in dist vma");
    
    MPI_Gather(MPI_IN_PLACE, 1, MPI_UINT64_T, slaves_space, 1, MPI_UINT64_T, 0, MPI_COMM_WORLD);
    
    /* sum everything */
    size_t total_avail = 0;
    for (int i = 0; i < world_size; i++) {
        total_avail += slaves_space[i];
    }
    
    /* if we don't have enough swap space on the slaves */
    if (total_avail < total_needed) {
        DEBUG_PRINTLN("Not enough swap on slaves: %lu vs %lu", total_avail, total_needed);
        free(slaves_space);
        shut_down_slaves(SHUTDOWN_WITH_SCATTER);
        return 0;
    }
    
    /* now try to map entire virtual range */
    if (!map_vma_range(num_units)) {
        DEBUG_PRINTLN("Could not map VMA range");
        free(slaves_space);
        shut_down_slaves(SHUTDOWN_WITH_SCATTER);
        return 0;
    }
    
    /* create structure for holding slave metadata */
    slaves_meta = malloc((world_size - 1) * sizeof(netram_slaves_t));
    for (int i = 0; i < world_size - 1; i++) {
        slaves_meta[i].rank = i + 1;
        slaves_meta[i].addr = 0;
        slaves_meta[i].size = slaves_space[i + 1];
    }
    
    free(slaves_space);
    
    /* sort slave capacities in ascending order */
    qsort(slaves_meta, world_size - 1, sizeof(netram_slaves_t), compare);
    void * curr_addr = base_data_addr;
    uint64_t size_left = total_needed;
    
    /* now distribute the space greedily */
    for (int i = 0; i < world_size - 1; i++) {
        size_t per_node = UNIT_ALIGN(size_left / (world_size - i - 1));
        slaves_meta[i].size = per_node < slaves_meta[i].size ? per_node : slaves_meta[i].size;
        slaves_meta[i].addr = curr_addr;
        curr_addr += slaves_meta[i].size;
        size_left -= slaves_meta[i].size;
    }
    
    /* sanity check, make sure its all been shared btw slaves */
    if (size_left != 0) {
        DEBUG_PRINTLN("Distribution got weird: %lu", size_left);
        free(slaves_meta);
        shut_down_slaves(SHUTDOWN_WITH_SCATTER);
        return 0;
    }
    
    /* construct messages for slaves,
       let them know their respective VMA */
    netram_msg_t * msgs = calloc(world_size, sizeof(netram_msg_t));
    if (msgs == NULL) {
        HANDLE_EXIT("malloc failed in dist vma");
    }
    for (int i = 0; i < world_size - 1; i++) {
        msgs[slaves_meta[i].rank].addr = (uint64_t)slaves_meta[i].addr;
        msgs[slaves_meta[i].rank].size = slaves_meta[i].size;
    }
    
    /* scatter their metadata */
    DEBUG_PRINTLN("Scattering slave metadata.");
    MPI_Scatter(msgs, 1, MPI_Netram_Type, MPI_IN_PLACE, 0, 0, 0, MPI_COMM_WORLD);
    free(msgs);
    
    return total_needed;
}

/* Used to tell the slaves to stop listening and shut down */
static void shut_down_slaves(int how) {
    if (slaves_already_stopped)
        return;
    
    DEBUG_PRINTLN("Sending shut down packets.");
    
    /* send shut down message with a scatter or with regular sends */
    if (how == SHUTDOWN_WITH_SCATTER) {
        netram_msg_t * msgs = calloc(world_size, sizeof(netram_msg_t));
        if (msgs == NULL) {
            HANDLE_EXIT("malloc failed in dist vma");
        }
        MPI_Scatter(msgs, 1, MPI_Netram_Type, MPI_IN_PLACE, 0, 0, 0, MPI_COMM_WORLD);
        free(msgs);
    }
    else {
        netram_msg_t msg = {0, 0};
        msg.addr = 0; msg.size = 0;
        for (int rank = 1; rank < world_size; rank++) {
            MPI_Send(&msg, 1, MPI_Netram_Type, rank, 0, MPI_COMM_WORLD);
        }
    }
    
    slaves_already_stopped = true;
}

/* maps the VMA for the block metadata as well as for the actual data */
static bool map_vma_range(size_t num_units) {
    total_free_data = num_units * NETRAM_UNIT_SZ;
    base_data_addr = mmap(NULL, total_free_data, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    
    if (base_data_addr == MAP_FAILED) {
        DEBUG_PRINTLN("data mmap failed.");
        return false;
    }
    
    /* allocate space for metadata */
    total_free_meta = num_units * (BLOCK_META_SIZE);
    base_meta_addr = mmap(NULL, total_free_meta, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    
    if (base_meta_addr == MAP_FAILED) {
        DEBUG_PRINTLN("meta mmap failed.");
        return false;
    }
    
    /* we dont need any physical pages yet */
    if (madvise(base_data_addr, total_free_data, MADV_DONTNEED) == -1) {
        DEBUG_PRINTLN("madvise failed in init");
        return false;
    }
    
    if (mprotect(base_data_addr, total_free_data, PROT_NONE) == -1) {
        HANDLE_EXIT("mprotect failed in handler (after swap out).");
    }
    
    return true;
}

static void * find_free_block(size_t size) {    
    /* go through the linked list and 
    find a free block of the right size */
    block_meta_t * current = base_meta_addr;
    while (current && !(current->free && current->size >= size)) {
        current = current->next;
    }
    return current;
}

void * netram_malloc(size_t size) {
    if (size == 0)
        return NULL;
    
    /* if its too small, pass it to malloc() */
    if (size < NETRAM_UNIT_SZ)
        return malloc(size);
    
    /* align it to the unit */
    size_t aligned_sz = UNIT_ALIGN(size);
    
    /* try to find free block with the right size */
    block_meta_t * block = find_free_block(size);
    
    if (!block) {
        DEBUG_PRINTLN("Found no free block");
        return NULL;
    }
    
    /* we found a free block, so claim it */
    block->free = false;
    block->ever_swapped = false;
    
    assert(aligned_sz % NETRAM_UNIT_SZ == 0);
    size_t num_units = aligned_sz / NETRAM_UNIT_SZ;
    
    /* Check if the free block is bigger than we actually need,
       Split it, if so */
    if (block->size > aligned_sz) {
        block_meta_t * other = block + num_units;
        
        other->size = block->size - aligned_sz;
        other->next = block->next;
        other->free = true;
        other->ever_swapped = false;
        
        block->next = other;
    }
    
    block->size = size;
    
    /* sanity check */
    void * block_addr = block;
    assert((block_addr - base_meta_addr) % BLOCK_META_SIZE == 0);
    
    /* calculate the data block that will be returned to the caller */
    size_t offset_units = (block_addr - base_meta_addr) / BLOCK_META_SIZE;
    size_t offset_bytes = offset_units * NETRAM_UNIT_SZ;
    return base_data_addr + offset_bytes;
}

void netram_free(void * ptr) {
    if (!ptr)
        return;
    
    block_meta_t * current = base_meta_addr;
    block_meta_t * prev = NULL;
    
    /* look for the block */
    while (current) {
        size_t offset = ((void *)current - base_meta_addr) / BLOCK_META_SIZE;
        void * data_blk = base_data_addr + (offset * NETRAM_UNIT_SZ);
        /* when we find it */
        if (data_blk == ptr) {
            current->free = true;
            current->ever_swapped = false;
            
            /* guarantee that freed block sizes are always block-aligned */
            current->size = (current->size + NETRAM_UNIT_SZ - 1) & ~(NETRAM_UNIT_SZ - 1);
            
            /* free physical pages and lock up the space */
            if (madvise(data_blk, current->size, MADV_DONTNEED) == -1) {
                HANDLE_EXIT("madvise failed in free");
            }
            
            if (mprotect(data_blk, current->size, PROT_NONE) == -1) {
                HANDLE_EXIT("mprotect failed in free");
            }
            
            return;
        }
        
        /* try to coalesce contiguous free blocks at the same time */
        if (prev && prev->free && current->free) {
            prev->size += current->size;
            prev->next = current->next;
            
            current->size = 0;
            current = current->next;
        }
        else {
            prev = current;
            current = current->next;
        }
    }
    
    /* if we didn't find it, pass it on to free() */
    free(ptr);
}

/* Just a useful function to dump block metadata */
void netram_dump(void) {
    block_meta_t * current = base_meta_addr;
    
    while (current != NULL) {
        DEBUG_PRINTLN("Size: %lu, Next: %p, Free: %d, Ever swapped: %d",
                    current->size, current->next, current->free, current->ever_swapped);
        current = current->next;
    }
    
    DEBUG_PRINTLN();
}

/* Clean up function */
void netram_end(void) {
    /* restore default SEGV handler */
    signal(SIGSEGV, SIG_DFL);
    
    /* unmap regions */
    free(stack_space);
    free(slaves_meta);
    munmap(base_data_addr, total_free_data);
    munmap(base_meta_addr, total_free_meta);
    base_data_addr = NULL;
    base_meta_addr = NULL;
    
    /* clean up MPI */
    int initialized, finalized;

    MPI_Initialized(&initialized);
    MPI_Finalized(&finalized);
    
    if (!initialized || finalized)
        return;
    
    shut_down_slaves(SHUTDOWN_WITH_SEND);
    
    MPI_Type_free(&MPI_Netram_Type);
    MPI_Finalize();
}

void setup_segv_handler(void) {
    /* allocate alternate stack space */
    stack_space = malloc(SIGSTKSZ);
    if (stack_space == NULL) {
        HANDLE_EXIT("Stack malloc failed");
    }
    
    /* set up the stack */
    static stack_t stack;
    stack.ss_sp = stack_space;
    stack.ss_size = SIGSTKSZ;
    stack.ss_flags = 0;
    
    if (sigaltstack (&stack, NULL)) {
        HANDLE_EXIT("Signal stack setup failed.");
    }
    
    DEBUG_PRINTLN("Signal stack setup success.\n");
    
    struct sigaction action;
    
    /* set up handler, so we run on alternate stack and take a siginfo_t */
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset (&action.sa_mask);
    
    /* block most signals when handling SIGSEGV */
    sigaddset(&action.sa_mask, SIGHUP);
    sigaddset(&action.sa_mask, SIGINT);
    sigaddset(&action.sa_mask, SIGQUIT);
    sigaddset(&action.sa_mask, SIGPIPE);
    sigaddset(&action.sa_mask, SIGALRM);
    sigaddset(&action.sa_mask, SIGTERM);
    sigaddset(&action.sa_mask, SIGUSR1);
    sigaddset(&action.sa_mask, SIGUSR2);
    sigaddset(&action.sa_mask, SIGCHLD);
    sigaddset(&action.sa_mask, SIGCLD);
    sigaddset(&action.sa_mask, SIGURG);
    sigaddset(&action.sa_mask, SIGIO);
    sigaddset(&action.sa_mask, SIGPOLL);
    sigaddset(&action.sa_mask, SIGXCPU);
    sigaddset(&action.sa_mask, SIGXFSZ);
    sigaddset(&action.sa_mask, SIGVTALRM);
    sigaddset(&action.sa_mask, SIGPROF);
    sigaddset(&action.sa_mask, SIGPWR);
    sigaddset(&action.sa_mask, SIGWINCH);

    action.sa_sigaction = segv_handle;
    
    if (sigaction(SIGSEGV, &action, NULL) == -1) {
        HANDLE_EXIT("Failed to setup SIGSEGV handler");
    }
}

/* SEGV handler, performs swapping and (un)locking of regions */
void segv_handle(int sig, siginfo_t *info, void *vcontext) {
    /* save errno */
    int temp_errno = errno;
    
    segv_happened++;
    void * segv_address = info->si_addr;
    
    //DEBUG_PRINTLN("\nFrom handler(%d): Fault address = 0x%lX", segv_happened, info->si_addr);
    
    size_t offset_units;
    size_t offset_bytes;
    void * start_addr, *end_addr;
    
    /* look for the parent block of the faulting address */
    block_meta_t * current = base_meta_addr;
    while (current) {
        offset_units = ((void *)current - base_meta_addr) / BLOCK_META_SIZE;
        offset_bytes = offset_units * NETRAM_UNIT_SZ;
        start_addr = base_data_addr + offset_bytes;
        end_addr = start_addr + current->size;
        
        /* check if the faulting address is within this block */
        if (segv_address >= start_addr && segv_address < end_addr) {
            break;
        }
        
        current = current->next;
    }
    
    /* if we didn't find it within our VMA, or its a freed block, re-raise SIGSEGV  */
    if (current == NULL || current->free) {
        DEBUG_PRINTLN("Out of range: %p, %d", current, current->free);
        signal(SIGSEGV, SIG_DFL);
        return;
    }
    
    /* calculate what is left within the block */
    size_t offset = (segv_address - start_addr) & ~(NETRAM_UNIT_SZ - 1);
    void * chunk_addr = (void *)start_addr + offset;
    end_addr = (void *)(UNIT_ALIGN((uint64_t)end_addr));
    size_t left = end_addr - chunk_addr;
    
    /* just to make sure */
    assert(NETRAM_MAX_TRANSFER_BYTES % NETRAM_UNIT_SZ == 0);
    
    /* maximum size we would like to load */
    size_t should_load = MIN(NETRAM_MAX_TRANSFER_BYTES, left);
    /* maximum size we can actually load, based on how much active memory is free */
    size_t can_load = MIN(NETRAM_MAX_ACTIVE_MEM - curr_active_sz, should_load);
    
    /* if we have any free active memory at all */
    if (can_load) {
        /* unlock the space then */
        if (mprotect(chunk_addr, can_load, PROT_READ | PROT_WRITE | PROT_EXEC) == -1) {
            HANDLE_EXIT("mprotect failed in handler");
        }
        
        if (madvise(chunk_addr, can_load, MADV_NORMAL) == -1) {
            HANDLE_EXIT("madvise failed in handler");
        }
        
        /* if necessary, swap in the chunk from slaves first */
        if (current->ever_swapped) {
            retrieve_units(chunk_addr, can_load);
        }
        curr_active_sz += can_load;
        
        /* add new record to active fifo */
        active_block_record_t record = {current, chunk_addr, can_load};
        if (!fifo_enqueue(&active_fifo, &record)) {
            HANDLE_EXIT("Enqueue failed in handler");
        }
        
        /* restore errno */
        errno = temp_errno;
        return;
    }
    
    /* if we're here, it means active memory is full */
    active_block_record_t record;
    size_t cleared = 0;
    /* so iterate thru queue to swap out oldest blocks */
    while (fifo_available(&active_fifo)) {
        if (!fifo_dequeue(&active_fifo, &record)) {
            HANDLE_EXIT("Dequeue failed in handler");
        }
        
        block_meta_t * parent = record.block;
        if (parent->size == 0 || record.size == 0)
            continue;
        
        /* if the parent block has been free()d previously */
        if (parent->free) {
            cleared += parent->size;
        }
        else {
            /* swap out this record's chunk to make room */
            swap_out_units(record.unit, record.size);
            cleared += record.size;
            parent->ever_swapped = true;
            
            /* now lock the region */
            if (mprotect(record.unit, record.size, PROT_NONE) == -1) {
                HANDLE_EXIT("mprotect failed in handler (after swap out).");
            }
            
            if (madvise(record.unit, record.size, MADV_DONTNEED) == -1) {
                HANDLE_EXIT("madvise failed in handler (after swap out)");
            }
            
        }
        
        /* check if we now have enough free space */
        if (cleared >= should_load)
            break;
    }
    
    /* update size of used active memory */
    curr_active_sz -= cleared;
    
    /* if we somehow still dont have enough space, re-raise SIGSEGV */
    if (cleared < should_load) {
        DEBUG_PRINTLN("Insufficient active memory");
        signal(SIGSEGV, SIG_DFL);
        return;
    }
    
    /* now unlock the faulting virtual area */
    if (mprotect(chunk_addr, should_load, PROT_READ | PROT_WRITE | PROT_EXEC) == -1) {
        HANDLE_EXIT("mprotect failed in handler (after swap out).");
    }
    
    if (madvise(chunk_addr, should_load, MADV_NORMAL) == -1) {
        HANDLE_EXIT("madvise failed in handler (after swap out)");
    }
    
    /* retrieve its data from slaves */
    if (current->ever_swapped) {
        retrieve_units(chunk_addr, should_load);
    }
    curr_active_sz += should_load;
    
    /* add new record to active queue */
    record.block = current;
    record.unit = chunk_addr;
    record.size = should_load;
    if (!fifo_enqueue(&active_fifo, &record)) {
        HANDLE_EXIT("Enqueue failed in handler");
    }
    
    /* restore errno */
    errno = temp_errno;
}

/* Used to retrieve or swap in units from the slaves */
void retrieve_units(void * addr, size_t size) {
    netram_slaves_t * slave = NULL;
    netram_msg_t msg;
    
    size_t curr_sz = size;
    void * curr_addr = addr;
    
    /* repeat till we've retrieved everything from the slaves */
    while (curr_sz) {
        /* identify the slave holding this VMA address */
        for (int i = 0; i < world_size - 1; i++) {
            slave = &slaves_meta[i];
            if (curr_addr >= slave->addr && curr_addr < slave->addr + slave->size) {
                break;
            }
        }
        
        /* calculate how much we can read from this slave */
        void * end_addr = curr_addr + curr_sz;
        void * slave_end_addr = slave->addr + slave->size;
        uint64_t to_recv = end_addr <= slave_end_addr ? curr_sz : slave_end_addr - curr_addr;
        
        /* tell the slave what we're about to read */
        msg.addr = (uint64_t)curr_addr;
        msg.size = to_recv;
        MPI_Send(&msg, 1, MPI_Netram_Type, slave->rank, NETRAM_MSG_READ, MPI_COMM_WORLD);
        
        /* read in the chunk from the slave and update counters */
        #if defined(NETRAM_REUSE_BUFFER)
        MPI_Recv(mpi_buf, to_recv, MPI_CHAR, slave->rank, 0, MPI_COMM_WORLD, NULL);
        memcpy(curr_addr, mpi_buf, to_recv);
        #else
        MPI_Recv(curr_addr, to_recv, MPI_CHAR, slave->rank, 0, MPI_COMM_WORLD, NULL);
        #endif
        
        curr_sz -= to_recv;
        curr_addr += to_recv;
    }
}

/* Used to swap out units to the slaves */
void swap_out_units(void * addr, size_t size) {
    netram_slaves_t * slave = NULL;
    netram_msg_t msg;
    
    size_t curr_sz = size;
    void * curr_addr = addr;
    
    /* repeat till we've sent everything to the slaves */
    while (curr_sz) {
        /* identify the slave holding this VMA address */
        for (int i = 0; i < world_size - 1; i++) {
            slave = &slaves_meta[i];
            if (curr_addr >= slave->addr && curr_addr < slave->addr + slave->size) {
                break;
            }
        }
        
        /* calculate how much we can write to this slave */
        void * end_addr = curr_addr + curr_sz;
        void * slave_end_addr = slave->addr + slave->size;
        uint64_t to_send = end_addr <= slave_end_addr ? curr_sz : slave_end_addr - curr_addr;
        
        /* tell the slave what we're about to write */
        msg.addr = (uint64_t)curr_addr;
        msg.size = to_send;
        MPI_Send(&msg, 1, MPI_Netram_Type, slave->rank, NETRAM_MSG_WRITE, MPI_COMM_WORLD);

        /* write the chunk to the slave and update counters */
        #if defined(NETRAM_REUSE_BUFFER)
        memcpy(mpi_buf, curr_addr, to_send);
        MPI_Send(mpi_buf, to_send, MPI_CHAR, slave->rank, 0, MPI_COMM_WORLD);
        #else
        MPI_Send(curr_addr, to_send, MPI_CHAR, slave->rank, 0, MPI_COMM_WORLD);
        #endif
        
        curr_sz -= to_send;
        curr_addr += to_send;
    }
}
