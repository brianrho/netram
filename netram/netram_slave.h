/* Written by Brian Ejike (2019)
 * Distributed under the MIT License */
 
#ifndef NETRAM_SLAVE_H_
#define NETRAM_SLAVE_H_

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

/* minimum allocatable and transferable unit; default 1 MB */
#define NETRAM_UNIT_SZ          (1 * 1024UL * 1024UL)

int netram_slave_init(void);
void netram_slave_loop(void);

#ifdef __cplusplus
}
#endif

#endif
