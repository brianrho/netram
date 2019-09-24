#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/time.h>

#include "netram_slave.h"

int main(void) {
    int rank;
    
    /* initialize VMAs and consult with master */
    if ((rank = netram_slave_init()) == -1) {
        return 0;
    }
    
    /* remain here and handle master messages */
    netram_slave_loop();
    
    return 0;
}