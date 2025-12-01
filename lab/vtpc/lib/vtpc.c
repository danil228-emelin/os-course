#include "vtpc.h"

#include <fcntl.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

#define BLOCK_SIZE 4096
#define CACHE_COUNT 16

typedef struct cache_page {
    off_t offset;
    void *data;
    bool dirty;
    struct cache_page *prev, *next;
} cache_page_t;

typedef struct cache {
    cache_page_t *head;
    cache_page_t *tail;
    size_t capacity;
    size_t size;
    size_t block_size;
    int real_fd;
} cache_t;

typedef struct file_descriptor {
    int fd;
    off_t offset;
    cache_t *cache;
    off_t file_size; 
} file_descriptor_t;

#define MAX_OPEN_FILES 256
static file_descriptor_t fd_table[MAX_OPEN_FILES];
static bool fd_table_initialized = false;

static void fd_table_init(void) {
    if (fd_table_initialized) return;
    
    for (int i = 0; i < MAX_OPEN_FILES; ++i) {
        fd_table[i].fd = -1;
        fd_table[i].offset = 0;
        fd_table[i].cache = NULL;
        fd_table[i].file_size = 0;
    }
    fd_table_initialized = true;
}

static cache_t *cache_init(size_t capacity, size_t block_size, int real_fd) {
    cache_t *cache = (cache_t *)malloc(sizeof(cache_t));
    if (!cache) return NULL;

    cache->head = NULL;
    cache->tail = NULL;
    cache->capacity = capacity;
    cache->size = 0;
    cache->block_size = block_size;
    cache->real_fd = real_fd;
    return cache;
}

static void cache_destroy(cache_t *cache) {
    if (!cache) return;
    
    cache_page_t *current = cache->head;
    while (current) {
        cache_page_t *next = current->next;
        free(current->data);
        free(current);
        current = next;
    }
    free(cache);
}

static void cache_promote(cache_t *cache, cache_page_t *page) {
    if (!cache || !page) return;
    if (cache->head == page) return;

    
    if (page->prev) page->prev->next = page->next;
    if (page->next) page->next->prev = page->prev;
    if (cache->tail == page) cache->tail = page->prev;

    
    page->prev = NULL;
    page->next = cache->head;
    if (cache->head) cache->head->prev = page;
    cache->head = page;
    if (!cache->tail) cache->tail = page;
}

static bool cache_write_back(cache_t *cache, cache_page_t *page) {
    if (!page->dirty) return true;
    
    ssize_t written = pwrite(cache->real_fd, page->data, cache->block_size, page->offset);
    if (written < 0) {
        return false;
    }
    page->dirty = false;
    return true;
}

static void cache_evict(cache_t *cache) {
    if (!cache || !cache->tail) return;

    cache_page_t *evicted = cache->tail;
    
    
    if (evicted->dirty) {
        cache_write_back(cache, evicted);
    }

    
    if (evicted->prev) {
        evicted->prev->next = NULL;
    } else {
        
        cache->head = NULL;
    }
    cache->tail = evicted->prev;

    
    free(evicted->data);
    free(evicted);
    cache->size--;
}

int vtpc_open(const char* path, int mode, int access) {
    if (!fd_table_initialized) {
        fd_table_init();
    }

    int real_fd = open(path, mode, access);
    if (real_fd < 0) {
        return -1;
    }

    struct stat st;
    if (fstat(real_fd, &st) < 0) {
        close(real_fd);
        return -1;
    }

    for (int i = 0; i < MAX_OPEN_FILES; ++i) {
        if (fd_table[i].fd == -1) {
            fd_table[i].fd = real_fd;
            fd_table[i].offset = 0;
            fd_table[i].file_size = st.st_size;
            fd_table[i].cache = cache_init(CACHE_COUNT, BLOCK_SIZE, real_fd);
            if (!fd_table[i].cache) {
                close(real_fd);
                return -1;
            }
            return i;
        }
    }

    close(real_fd);
    errno = EMFILE;
    return -1;
}

int vtpc_close(int fd) {
    if (fd < 0 || fd >= MAX_OPEN_FILES || fd_table[fd].fd == -1) {
        errno = EBADF;
        return -1;
    }

    file_descriptor_t *file = &fd_table[fd];
    cache_t *cache = file->cache;

    
    cache_page_t *page = cache->head;
    while (page) {
        cache_write_back(cache, page);
        page = page->next;
    }

    close(file->fd);
    cache_destroy(cache);
    
    fd_table[fd].fd = -1;
    fd_table[fd].cache = NULL;
    fd_table[fd].offset = 0;
    fd_table[fd].file_size = 0;
    
    return 0;
}

ssize_t vtpc_read(int fd, void *buf, size_t count) {
    if (fd < 0 || fd >= MAX_OPEN_FILES || fd_table[fd].fd == -1) {
        errno = EBADF;
        return -1;
    }
    if (count == 0) return 0;

    file_descriptor_t *file = &fd_table[fd];
    cache_t *cache = file->cache;
    size_t block_size = cache->block_size;

    
    if (file->offset >= file->file_size) {
        return 0; 
    }

    
    if (file->offset + (off_t)count > file->file_size) {
        count = file->file_size - file->offset;
    }

    size_t total_read = 0;
    char *dest = (char *)buf;
    
    while (count > 0) {
        off_t block_offset = (file->offset / (off_t)block_size) * (off_t)block_size;
        size_t block_index = file->offset % block_size;

        
        cache_page_t *page = cache->head;
        while (page) {
            if (page->offset == block_offset) {
                break;
            }
            page = page->next;
        }

        if (page) {
            cache_promote(cache, page);
        } else {
            
            if (cache->size >= cache->capacity) {
                cache_evict(cache);
            }

            page = (cache_page_t *)malloc(sizeof(cache_page_t));
            if (!page) {
                errno = ENOMEM;
                return -1;
            }

            page->data = malloc(block_size);
            if (!page->data) {
                free(page);
                errno = ENOMEM;
                return -1;
            }

            
            off_t file_remaining = file->file_size - block_offset;
            size_t to_read = (file_remaining < (off_t)block_size) ? 
                             (size_t)file_remaining : block_size;
            
            ssize_t read_bytes = pread(file->fd, page->data, to_read, block_offset);
            if (read_bytes < 0) {
                free(page->data);
                free(page);
                return -1;
            }
            
            
            if (read_bytes < (ssize_t)block_size) {
                memset((char*)page->data + read_bytes, 0, block_size - read_bytes);
            }

            page->offset = block_offset;
            page->dirty = false;
            page->prev = NULL;
            page->next = cache->head;
            if (cache->head) cache->head->prev = page;
            cache->head = page;
            if (!cache->tail) cache->tail = page;
            cache->size++;
        }

        
        size_t available_in_block = block_size - block_index;
        size_t available_in_file = file->file_size - file->offset;
        size_t to_copy = count;
        
        if (to_copy > available_in_block) to_copy = available_in_block;
        if (to_copy > available_in_file) to_copy = available_in_file;
        
        memcpy(dest + total_read, (char*)page->data + block_index, to_copy);
        
        total_read += to_copy;
        count -= to_copy;
        file->offset += to_copy;
    }

    return total_read;
}

ssize_t vtpc_write(int fd, const void *buf, size_t count) {
    if (fd < 0 || fd >= MAX_OPEN_FILES || fd_table[fd].fd == -1) {
        errno = EBADF;
        return -1;
    }
    if (count == 0) return 0;

    file_descriptor_t *file = &fd_table[fd];
    cache_t *cache = file->cache;
    size_t block_size = cache->block_size;

    const char *src = (const char *)buf;
    size_t bytes_written = 0;

    
    if (file->offset + (off_t)count > file->file_size) {
        file->file_size = file->offset + count;
    }

    while (count > 0) {
        off_t block_offset = (file->offset / (off_t)block_size) * (off_t)block_size;
        size_t block_index = file->offset % block_size;

        
        cache_page_t *page = cache->head;
        while (page) {
            if (page->offset == block_offset) break;
            page = page->next;
        }

        if (!page) {
            
            if (cache->size >= cache->capacity) {
                cache_evict(cache);
            }

            page = (cache_page_t *)malloc(sizeof(cache_page_t));
            if (!page) {
                errno = ENOMEM;
                return -1;
            }

            page->data = malloc(block_size);
            if (!page->data) {
                free(page);
                errno = ENOMEM;
                return -1;
            }

            
            if (block_offset < file->file_size) {
                off_t file_remaining = file->file_size - block_offset;
                size_t to_read = (file_remaining < (off_t)block_size) ? 
                                 (size_t)file_remaining : block_size;
                
                ssize_t read_bytes = pread(file->fd, page->data, to_read, block_offset);
                if (read_bytes < 0 && errno != EINVAL) {
                    
                    memset(page->data, 0, block_size);
                } else if (read_bytes >= 0 && read_bytes < (ssize_t)block_size) {
                    memset((char*)page->data + read_bytes, 0, block_size - read_bytes);
                } else {
                    memset(page->data, 0, block_size);
                }
            } else {
                
                memset(page->data, 0, block_size);
            }

            page->offset = block_offset;
            page->dirty = false;
            page->prev = NULL;
            page->next = cache->head;
            if (cache->head) cache->head->prev = page;
            cache->head = page;
            if (!cache->tail) cache->tail = page;
            cache->size++;
        }

        cache_promote(cache, page);

        
        size_t to_copy = (count > block_size - block_index) ? block_size - block_index : count;
        memcpy((char*)page->data + block_index, src + bytes_written, to_copy);
        page->dirty = true;

        bytes_written += to_copy;
        count -= to_copy;
        file->offset += to_copy;
    }

    return bytes_written;
}

off_t vtpc_lseek(int fd, off_t offset, int whence) {
    if (fd < 0 || fd >= MAX_OPEN_FILES || fd_table[fd].fd == -1) {
        errno = EBADF;
        return -1;
    }

    file_descriptor_t *file = &fd_table[fd];
    off_t new_offset;

    switch (whence) {
        case SEEK_SET:
            new_offset = offset;
            break;
        case SEEK_CUR:
            new_offset = file->offset + offset;
            break;
        case SEEK_END: {
            
            new_offset = file->file_size + offset;
            break;
        }
        default:
            errno = EINVAL;
            return -1;
    }

    if (new_offset < 0) {
        errno = EINVAL;
        return -1;
    }
    
    file->offset = new_offset;
    return new_offset;
}

int vtpc_fsync(int fd) {
    if (fd < 0 || fd >= MAX_OPEN_FILES || fd_table[fd].fd == -1) {
        errno = EBADF;
        return -1;
    }

    file_descriptor_t *file = &fd_table[fd];
    cache_t *cache = file->cache;
    
    
    cache_page_t *page = cache->head;
    while (page) {
        if (page->dirty) {
            if (!cache_write_back(cache, page)) {
                return -1;
            }
        }
        page = page->next;
    }

    
    if (ftruncate(file->fd, file->file_size) < 0) {
        return -1;
    }

    return fsync(file->fd);
}