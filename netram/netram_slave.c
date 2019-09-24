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
#include <limits.h>

#include <mpi.h>
#include <assert.h>
#include <math.h>

#include "netram_slave.h"

/* msg tags for indicating MPI R/W operation from master */
#define NETRAM_MSG_WRITE    (1)
#define NETRAM_MSG_READ     (2)

/* align some pointer or size to the unit size */ 
#define UNIT_ALIGN(x)   (((x) + NETRAM_UNIT_SZ - 1) & ~(NETRAM_UNIT_SZ - 1))

static size_t commit_limit = 0;
static size_t commited_as = 0;
static uint64_t avail_mem = 0;
static uint64_t actual_mem = 0;

/* backing storage and the base address of this slave's portion of the total VMA */
static void * storage = NULL;
static uint64_t base_addr = 0;

static int world_rank = 0;
static int world_size = 0;

/* clearance so we dont use up all the uncommitted memory */
static const size_t CLEARANCE = 2UL * 1024 * 1024 * 1024;

/* used to pass messages between master and slaves */
typedef struct {
    uint64_t addr;
    uint64_t size;
} netram_msg_t;

/* get the amount of committed memory as well as the commit limit */
static bool get_avail_commit(void) {
    FILE *meminfo = fopen("/proc/meminfo", "r");
    
    if (meminfo == NULL) 
        return false;
    
    char * line = NULL;
    size_t len = 0;
    int nread;
    
    int parsed = 0;
    while ((nread = getline(&line, &len, meminfo)) != -1) {
        if (sscanf(line, "CommitLimit: %lu kB", &commit_limit) == 1) {
            parsed++;
            continue;
        }
        if (sscanf(line, "Committed_AS: %lu kB", &commited_as) == 1) {
            parsed++;
            continue;
        }
    }
    
    /* convert to bytes */
    commit_limit *= 1024;
    commited_as *= 1024;
    
    free(line);
    fclose(meminfo);
    
    return parsed == 2;
}

int netram_slave_init(void) {
    int initialized;
    
    MPI_Initialized(&initialized);
    if (!initialized) {
        MPI_Init(NULL, NULL);
    }
    
    MPI_Comm_rank(MPI_COMM_WORLD, &world_rank);
	MPI_Comm_size(MPI_COMM_WORLD, &world_size);
    
    /* get committed memory */
    if (!get_avail_commit()) {
        DEBUG_PRINTLN("Could not get uncommitted mem.");
        avail_mem = 0;
    }
    else {
        /* calculate how much we can ask for */
        if (commit_limit < commited_as || commit_limit - commited_as < CLEARANCE)
            avail_mem = 0;
        else 
            avail_mem = commit_limit - commited_as - CLEARANCE;
    }
    
    /* round down to unit boundary */
    avail_mem =  avail_mem & ~(NETRAM_UNIT_SZ - 1);
    
    /* try to grab the entire range now */
    storage = mmap(NULL, avail_mem, PROT_READ | PROT_WRITE, 
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    
    /* if we could not map the space, raise an error */
    if (storage == MAP_FAILED) {
        DEBUG_PRINTLN("%d: VMA map failed", world_rank);
        avail_mem = 0;
    }
    
    /* inform master of available memory */
    int err = MPI_Gather(&avail_mem, 1, MPI_UINT64_T, NULL, 0, 0, 0, MPI_COMM_WORLD);
    
    /* create MPI type for msg */
    MPI_Datatype MPI_Netram_Type;
    MPI_Type_contiguous(2, MPI_UINT64_T, &MPI_Netram_Type);
    MPI_Type_commit(&MPI_Netram_Type);
    
    /* find out how much space is actually needed, recvd size must be unit-aligned */
    netram_msg_t msg = {0, 0};
    MPI_Scatter(NULL, 0, 0, &msg, 1, MPI_Netram_Type, 0, MPI_COMM_WORLD);
    MPI_Type_free(&MPI_Netram_Type);
    
    /* save our portion of the VMA, make sure to align size */
    base_addr = msg.addr;
    actual_mem = msg.size;
    actual_mem =  actual_mem & ~(NETRAM_UNIT_SZ - 1);
    
    /* if this slave isnt needed after all or something else went wrong */
    if (actual_mem == 0) {
        munmap(storage, avail_mem);
        MPI_Finalize();
        return -1;
    }
    
    DEBUG_PRINTLN("%d: Avail: %lu B, Actual: %lu B, Base address: %p", world_rank, avail_mem, actual_mem, base_addr);
    /* unmap the unneeded region from before */
    munmap(storage + actual_mem, avail_mem - actual_mem);
    return world_rank;
}

/* This is the main program loop and will run forever */
void netram_slave_loop(void) {
    netram_msg_t msg;
    
    /* create MPI type for msg */
    MPI_Datatype MPI_Netram_Type;
    MPI_Type_contiguous(2, MPI_UINT64_T, &MPI_Netram_Type);
    MPI_Type_commit(&MPI_Netram_Type);
    
    MPI_Status stat;
    
    while (true) {
        /* block till we get a request */
        MPI_Recv(&msg, 1, MPI_Netram_Type, 0, MPI_ANY_TAG, MPI_COMM_WORLD, &stat);
        
        /* we're done or some badness happened */
        if (msg.size == 0 || msg.addr == 0)
            break;
        
        void * real_addr = storage + (msg.addr - base_addr);
        
        /* check if the master wants to read or write from this slave */
        if (stat.MPI_TAG == NETRAM_MSG_READ) {
            MPI_Send(real_addr, msg.size, MPI_CHAR, 0, 0, MPI_COMM_WORLD);
        }
        else if (stat.MPI_TAG == NETRAM_MSG_WRITE) {
            MPI_Recv(real_addr, msg.size, MPI_CHAR, 0, 0, MPI_COMM_WORLD, NULL);
        }
        else {
            /* something went wrong */
            break;
        }
    }
    
    DEBUG_PRINTLN("%d: Shutting down.", world_rank);
    
    /* unmap, not so necessary since we're exiting anyways */
    munmap(storage, actual_mem);
    MPI_Type_free(&MPI_Netram_Type);
    
    int finalized;
    MPI_Finalized(&finalized);
    if (!finalized)
        MPI_Finalize();
}
