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

    // Удаляем из текущей позиции
    if (page->prev) page->prev->next = page->next;
    if (page->next) page->next->prev = page->prev;
    if (cache->tail == page) cache->tail = page->prev;

    // Вставляем в начало
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
    
    // Записываем на диск если грязная
    if (evicted->dirty) {
        cache_write_back(cache, evicted);
    }

    // Удаляем из списка
    if (evicted->prev) {
        evicted->prev->next = NULL;
    } else {
        // Это единственная страница
        cache->head = NULL;
    }
    cache->tail = evicted->prev;

    // Освобождаем память
    free(evicted->data);
    free(evicted);
    cache->size--;
}

int vtpc_open(const char* path, int mode, int access) {
    if (!fd_table_initialized) {
        fd_table_init();
    }

    // Убираем O_DIRECT для упрощения
    int real_fd = open(path, mode, access);
    if (real_fd < 0) {
        return -1;
    }

    for (int i = 0; i < MAX_OPEN_FILES; ++i) {
        if (fd_table[i].fd == -1) {
            fd_table[i].fd = real_fd;
            fd_table[i].offset = 0;
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

    // Сбрасываем все грязные страницы
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

    size_t total_read = 0;
    char *dest = (char *)buf;
    
    while (count > 0) {
        off_t block_offset = (file->offset / (off_t)block_size) * (off_t)block_size;
        size_t block_index = file->offset % block_size;

        // Ищем страницу в кэше
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
            // Страницы нет в кэше
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

            // Читаем с диска
            ssize_t read_bytes = pread(file->fd, page->data, block_size, block_offset);
            if (read_bytes < 0) {
                free(page->data);
                free(page);
                return -1;
            }
            
            // Заполняем нулями если прочитали меньше
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

        // Копируем данные из страницы
        size_t available = block_size - block_index;
        size_t to_copy = (count > available) ? available : count;
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

    while (count > 0) {
        off_t block_offset = (file->offset / (off_t)block_size) * (off_t)block_size;
        size_t block_index = file->offset % block_size;

        // Ищем страницу в кэше
        cache_page_t *page = cache->head;
        while (page) {
            if (page->offset == block_offset) break;
            page = page->next;
        }

        if (!page) {
            // Страницы нет - создаем новую или вытесняем старую
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

            // Читаем существующие данные с диска
            ssize_t read_bytes = pread(file->fd, page->data, block_size, block_offset);
            if (read_bytes < 0 && errno != EINVAL) {
                // EINVAL может быть если блок за пределами файла
                memset(page->data, 0, block_size);
            } else if (read_bytes < (ssize_t)block_size) {
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

        cache_promote(cache, page);

        // Копируем данные в страницу
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
    struct stat st;

    switch (whence) {
        case SEEK_SET:
            new_offset = offset;
            break;
        case SEEK_CUR:
            new_offset = file->offset + offset;
            break;
        case SEEK_END:
            if (fstat(file->fd, &st) < 0) return -1;
            new_offset = st.st_size + offset;
            break;
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
    
    // Сбрасываем все грязные страницы
    cache_page_t *page = cache->head;
    while (page) {
        if (page->dirty) {
            if (!cache_write_back(cache, page)) {
                return -1;
            }
        }
        page = page->next;
    }

    return fsync(file->fd);
}