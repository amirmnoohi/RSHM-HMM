#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <string.h>

#define DEVICE_PATH "/dev/mmap_dev"
#define DEVICE_SIZE (4 * getpagesize())

int main() {
    int fd = open(DEVICE_PATH, O_RDWR);
    if (fd == -1) {
        perror("open");
        return 1;
    }

    void *map = mmap(NULL, DEVICE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return 1;
    }
    //print pid
    printf("pid: %d\n", getpid());

    //print map
    printf("map: %p\n", map);

    printf("Read from device: %s\n", (char *)map);

    char *message = "Hello, mmap_dev!";
    memcpy(map, message, strlen(message) + 1);

    //sleep 5 seconds
    sleep(5);

    printf("Read from device: %s\n", (char *)map);

    if (munmap(map, DEVICE_SIZE) == -1) {
        perror("munmap");
        close(fd);
        return 1;
    }

    close(fd);
    return 0;
}