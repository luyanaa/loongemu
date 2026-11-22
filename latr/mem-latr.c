/* LATR-local allocator shim.
 *
 * The freestanding LATR core only needs malloc/calloc/realloc/free wrappers.
 * It used to compile target/arm/lata/mem.c, but that file now carries the
 * ARM guest-memory probing code and pulls QEMU target headers that are not
 * part of the LATR include set. Keeping a local copy avoids coupling the
 * RISC-V translator to unrelated ARM work.
 */
#include <stdlib.h>

void *mm_malloc(int size)
{
    return malloc(size);
}

void *mm_calloc(int num, int size)
{
    return calloc(num, size);
}

void *mm_realloc(void *ptr, int size)
{
    return realloc(ptr, size);
}

void mm_free(void *ptr)
{
    free(ptr);
}

void *mmi_palloc(int size, char *file_name, int line_num)
{
    (void)file_name;
    (void)line_num;
    return malloc(size);
}
