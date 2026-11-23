#include "vtpc.h"

#include <fcntl.h>       // Основной заголовок для open, O_DIRECT
#include <unistd.h>      // Для close, read, write, pread, pwrite
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>    // Для fstat
#include <errno.h>


#ifndef O_DIRECT
#define O_DIRECT 00040000 /* direct disk access hint */
#endif

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
    int real_fd;  // Добавляем реальный файловый дескриптор
} cache_t;

typedef struct file_descriptor {
    int fd;
    off_t offset;
    cache_t *cache;
} file_descriptor_t;

#define MAX_OPEN_FILES 256
static file_descriptor_t fd_table[MAX_OPEN_FILES];
static bool fd_table_initialized = false;

// Инициализация таблицы файлов
static void fd_table_init(void) {
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
    cache->real_fd = real_fd;  // Сохраняем реальный fd
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

    // Удаляем страницу из текущей позиции
    if (page->prev) page->prev->next = page->next;
    if (page->next) page->next->prev = page->prev;
    if (cache->tail == page) cache->tail = page->prev;

    // Перемещаем в начало
    page->prev = NULL;
    page->next = cache->head;
    if (cache->head) cache->head->prev = page;
    cache->head = page;

    if (!cache->tail) cache->tail = page;
}

static void cache_evict(cache_t *cache) {
    if (!cache || !cache->tail) return;

    // Evict из хвоста (LRU политика)
    cache_page_t *evicted = cache->tail;

    if (evicted->dirty) {
        // Используем правильный файловый дескриптор
        ssize_t written = pwrite(cache->real_fd, evicted->data, cache->block_size, evicted->offset);
        if (written < 0) {
            // Обработка ошибки записи
            perror("cache_evict: pwrite failed");
        }
    }

    // Удаляем из списка
    if (evicted->prev) {
        evicted->prev->next = NULL;
    }
    cache->tail = evicted->prev;
    
    if (!cache->tail) {
        cache->head = NULL;
    }

    free(evicted->data);
    free(evicted);
    cache->size--;
}

int vtpc_open(const char* path,int mode, int access) {
    // Инициализация таблицы при первом вызове
    if (!fd_table_initialized) {
        fd_table_init();
    }

    int real_fd = open(path, mode| O_DIRECT, access);
    if (real_fd < 0) {
        // Если O_DIRECT не поддерживается, пробуем без него
        real_fd = open(path, mode, access);
        if (real_fd < 0) {
            return -1;
        }
    }

    // Ищем свободный слот в таблице
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
    return -1;
}

int vtpc_close(int fd) {
    if (fd < 0 || fd >= MAX_OPEN_FILES || fd_table[fd].fd == -1) {
        errno = EBADF;
        return -1;
    }

    file_descriptor_t *file = &fd_table[fd];
    cache_t *cache = file->cache;

    // Сбрасываем все dirty страницы на диск
    cache_page_t *page = cache->head;
    while (page) {
        if (page->dirty) {
            pwrite(file->fd, page->data, cache->block_size, page->offset);
            page->dirty = false;
        }
        page = page->next;
    }

    // Закрываем реальный файл и очищаем кэш
    close(file->fd);
    cache_destroy(cache);
    
    // Очищаем запись в таблице
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
    
    while (count > 0) {
        off_t block_offset = file->offset / block_size * block_size;
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
            // Cache hit
            cache_promote(cache, page);
            size_t available = block_size - block_index;
            size_t to_copy = (count > available) ? available : count;
            memcpy((char*)buf + total_read, (char*)page->data + block_index, to_copy);
            total_read += to_copy;
            count -= to_copy;
            file->offset += to_copy;
        } else {
            // Cache miss
            if (cache->size >= cache->capacity) {
                cache_evict(cache);
            }

            page = (cache_page_t *)malloc(sizeof(cache_page_t));
            if (!page) return -1;

            if (posix_memalign(&page->data, block_size, block_size) != 0) {
                free(page);
                return -1;
            }

            // Читаем данные с диска
            ssize_t read_bytes = pread(file->fd, page->data, block_size, block_offset);
            if (read_bytes < 0) {
                free(page->data);
                free(page);
                return -1;
            }

            if (read_bytes == 0) {
                // Конец файла
                free(page->data);
                free(page);
                break;
            }

            // Заполняем оставшуюся часть нулями если нужно
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

            // Копируем данные
            size_t available = (read_bytes > block_index) ? read_bytes - block_index : 0;
            size_t to_copy = (count > available) ? available : count;
            if (to_copy > 0) {
                memcpy((char*)buf + total_read, (char*)page->data + block_index, to_copy);
                total_read += to_copy;
                count -= to_copy;
                file->offset += to_copy;
            }
            
            if (to_copy == 0) break; // Достигнут конец файла
        }
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

    size_t bytes_written = 0;

    while (count > 0) {
        off_t block_offset = file->offset / block_size * block_size;
        size_t block_index = file->offset % block_size;

        // Ищем страницу в кэше
        cache_page_t *page = cache->head;
        while (page) {
            if (page->offset == block_offset) break;
            page = page->next;
        }

        // Если страницы нет в кэше - загружаем или создаем
        if (!page) {
            if (cache->size >= cache->capacity) {
                cache_evict(cache);
            }

            page = (cache_page_t *)malloc(sizeof(cache_page_t));
            if (!page) return -1;

            if (posix_memalign(&page->data, block_size, block_size) != 0) {
                free(page);
                return -1;
            }

            // Пытаемся прочитать существующие данные
            ssize_t read_bytes = pread(file->fd, page->data, block_size, block_offset);
            if (read_bytes < 0 && read_bytes != -1) {
                free(page->data);
                free(page);
                return -1;
            }
            
            // Если ошибка или конец файла - заполняем нулями
            if (read_bytes <= 0) {
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

        size_t to_copy = (count > block_size - block_index) ? block_size - block_index : count;
        memcpy((char*)page->data + block_index, (const char*)buf + bytes_written, to_copy);
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
            struct stat st;
            if (fstat(file->fd, &st) < 0) return -1;
            new_offset = st.st_size + offset;
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

    // Сначала сбрасываем все dirty страницы
    file_descriptor_t *file = &fd_table[fd];
    cache_t *cache = file->cache;
    
    cache_page_t *page = cache->head;
    while (page) {
        if (page->dirty) {
            ssize_t written = pwrite(file->fd, page->data, cache->block_size, page->offset);
            if (written < 0) return -1;
            page->dirty = false;
        }
        page = page->next;
    }

    // Затем синхронизируем файл
    return fsync(file->fd);
}