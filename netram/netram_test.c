#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/time.h>
#include <time.h>

#include "netram.h"

int main(void) {
    printf("NETRAM test: \tAllocate some memory, write the same byte to each location and sum them.\n\n");
    
    /* this is the maximum amount of memory we intend to use,
       4 * 1024 UNITS, 1 UNIT = 1 MB, so a total of 4 GB */
    size_t num_units = 4UL * 1024;
    
    /* initialize netram */
    if (!netram_init(num_units)) {
        return 0;
    }
    fflush(stdout);
    
    printf("After INIT:\n");
    fflush(stdout);
    
    /* dump debug info */
    netram_dump();
    
    /* allocate the entire space */
    size_t size = num_units * NETRAM_UNIT_SZ;
    uint8_t * data = netram_malloc(size);
    
    printf("\nAfter MALLOC:\n");
    netram_dump();
    fflush(stdout);
    
    printf("\n\nWriting: \n");
    fflush(stdout);
    
    clock_t begin, end;
	begin = clock();
    uint8_t number = 2;
    
    /* write a byte to every location in the space */
    memset(data, number, size);
    end = clock();
    printf("Write time: %lf s\n", (double)(end - begin) / CLOCKS_PER_SEC);
    
    printf("\n\nReading: \n");
    fflush(stdout);
    
    begin = clock();
    /* read back those bytes and sum them */
    uint64_t total = 0;
    for (size_t i = 0; i < size; i++) {
        total += data[i];
    }
    end = clock();
    
    printf("Read time: %lf s\n\n", (double)(end - begin) / CLOCKS_PER_SEC);
    
    /* compare the expected total and what we actually got */
    printf("Expected total: %lu\n", number * size);
    printf("Actual total: %lu\n\n", total);
    
    /* free everything */
    netram_free(data);
    printf("After FREE:\n");
    netram_dump();
    fflush(stdout);
    
    /* cleanup */
    netram_end();
    
    return 0;
}