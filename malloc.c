#include <assert.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

// Don't include stdlb since the names will conflict?

#define ALIGNMENT 8
#define ALIGN(size) (((size) + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1))

// sbrk some extra space every time we need it.
// This does no bookkeeping and therefore has no ability to free, realloc, etc.
void *nofree_malloc(size_t size) {
    void *p       = sbrk(0);
    void *request = sbrk(size);
    if (request == (void *)-1) {
        return NULL;  // sbrk failed
    } else {
        assert(p == request);  // Not thread safe.
        return p;
    }
}

// sizeof(struct block_meta) 必须是 8 的倍数，
struct block_meta {
    size_t size;
    struct block_meta *next;
    struct block_meta *prev;
    int free;
    int magic;  // For debugging only. TODO: remove this in non-debug mode.
};

#define META_SIZE sizeof(struct block_meta)

void *global_base = NULL;

// Iterate through blocks until we find one that's large enough.
// TODO: split block up if it's larger than necessary
struct block_meta *find_free_block(struct block_meta **last, size_t size) {
    struct block_meta *current = global_base;
    while (current && !(current->free && current->size >= size)) {
        *last   = current;
        current = current->next;
    }
    return current;
}

struct block_meta *request_space(struct block_meta *last, size_t size) {
    struct block_meta *block;
    block         = sbrk(0);
    void *request = sbrk(size + META_SIZE);
    assert((void *)block == request);  // Not thread safe.
    if (request == (void *)-1) {
        return NULL;  // sbrk failed.
    }

    if (last) {  // NULL on first request.
        last->next = block;
    }
    block->prev  = last;
    block->size  = size;
    block->next  = NULL;
    block->free  = 0;
    block->magic = 0x12345678;
    return block;
}

void split_block(struct block_meta *block, size_t size) {
    if (block->size >= size + META_SIZE + ALIGNMENT) {
        struct block_meta *new_block = (struct block_meta *)((char *)(block + 1) + size);
        new_block->size  = block->size - size - META_SIZE;
        new_block->next  = block->next;
        new_block->prev  = block;
        new_block->free  = 1;
        new_block->magic = 0x55555555;

        if (new_block->next) {
            new_block->next->prev = new_block;
        }

        block->size = size;
        block->next = new_block;
    }
}

struct block_meta *merge_block(struct block_meta *block) {
    // 1. Merge with next if it is free
    if (block->next && block->next->free) {
        block->size += META_SIZE + block->next->size;
        block->next = block->next->next;
        if (block->next) {
            block->next->prev = block;
        }
    }

    // 2. Merge with prev if it is free
    if (block->prev && block->prev->free) {
        block->prev->size += META_SIZE + block->size;
        block->prev->next = block->next;
        if (block->next) {
            block->next->prev = block->prev;
        }
        block = block->prev;
    }

    return block;
}

// If it's the first ever call, i.e., global_base == NULL, request_space and set global_base.
// Otherwise, if we can find a free block, use it.
// If not, request_space.
void *malloc(size_t size) {
    struct block_meta *block;
    if (size <= 0) {
        return NULL;
    }

    size = ALIGN(size);

    if (!global_base) {  // First call.
        block = request_space(NULL, size);
        if (!block) {
            return NULL;
        }
        global_base = block;
    } else {
        struct block_meta *last = global_base;
        block                   = find_free_block(&last, size);
        if (!block) {  // Failed to find free block.
            block = request_space(last, size);
            if (!block) {
                return NULL;
            }
        } else {  // Found free block
            split_block(block, size);
            block->free  = 0;
            block->magic = 0x77777777;
        }
    }

    return (block + 1);
}

void *calloc(size_t nelem, size_t elsize) {
    size_t size = nelem * elsize;
    void *ptr   = malloc(size);
    memset(ptr, 0, size);
    return ptr;
}

// TODO: maybe do some validation here.
struct block_meta *get_block_ptr(void *ptr) {
    return (struct block_meta *)ptr - 1;
}

void free(void *ptr) {
    if (!ptr) {
        return;
    }

    struct block_meta *block_ptr = get_block_ptr(ptr);
    assert(block_ptr->free == 0);
    assert(block_ptr->magic == 0x77777777 || block_ptr->magic == 0x12345678);
    block_ptr->free  = 1;
    block_ptr->magic = 0x55555555;

    merge_block(block_ptr);
}

void *realloc(void *ptr, size_t size) {
    if (!ptr) {
        // NULL ptr. realloc should act like malloc.
        return malloc(size);
    }

    struct block_meta *block_ptr = get_block_ptr(ptr);
    if (block_ptr->size >= size) {
        // We have enough space. Could free some once we implement split.
        return ptr;
    }

    // Need to really realloc. Malloc new space and free old space.
    // Then copy old data to new space.
    void *new_ptr;
    new_ptr = malloc(size);
    if (!new_ptr) {
        return NULL;  // TODO: set errno on failure.
    }
    memcpy(new_ptr, ptr, block_ptr->size);
    free(ptr);
    return new_ptr;
}
