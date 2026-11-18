#ifndef LATX_BARE_UNISTD_H
#define LATX_BARE_UNISTD_H
#include <stddef.h>
typedef long ssize_t;
typedef int pid_t;
pid_t getpid(void);
unsigned int sleep(unsigned int seconds);
int usleep(unsigned int usec);
ssize_t write(int fd, const void *buf, size_t n);
ssize_t read(int fd, void *buf, size_t n);
int close(int fd);
void _exit(int code);
#endif
