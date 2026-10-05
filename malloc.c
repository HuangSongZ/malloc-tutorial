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

struct block_meta {
    size_t size;
    struct block_meta *next;        // Physical next block
    struct block_meta *prev;        // Physical prev block
    struct block_meta *next_free;   // Explicit free list next
    struct block_meta *prev_free;   // Explicit free list prev
    int free;
    int magic;  // For debugging only. TODO: remove this in non-debug mode.
};

#define META_SIZE sizeof(struct block_meta)

void *global_base = NULL;
struct block_meta *global_last = NULL;
struct block_meta *free_list_head = NULL;

void add_to_free_list(struct block_meta *block) {
    if (!block) return;
    block->next_free = free_list_head;
    block->prev_free = NULL;
    if (free_list_head) {
        free_list_head->prev_free = block;
    }
    free_list_head = block;
}

void remove_from_free_list(struct block_meta *block) {
    if (!block) return;
    if (block->prev_free) {
        block->prev_free->next_free = block->next_free;
    } else {
        if (free_list_head == block) {
            free_list_head = block->next_free;
        }
    }
    if (block->next_free) {
        block->next_free->prev_free = block->prev_free;
    }
    block->next_free = NULL;
    block->prev_free = NULL;
}

// Find free block by ONLY traversing the explicit free list
struct block_meta *find_free_block(size_t size) {
    struct block_meta *current = free_list_head;
    while (current && current->size < size) {
        current = current->next_free;
    }
    return current;
}

struct block_meta *request_space(size_t size) {
    struct block_meta *block;
    block         = sbrk(0);
    void *request = sbrk(size + META_SIZE);
    assert((void *)block == request);  // Not thread safe.
    if (request == (void *)-1) {
        return NULL;  // sbrk failed.
    }

    if (global_last) {
        global_last->next = block;
    }
    block->prev      = global_last;
    block->next      = NULL;
    block->next_free = NULL;
    block->prev_free = NULL;
    block->size      = size;
    block->free      = 0;
    block->magic     = 0x12345678;

    global_last = block;
    if (!global_base) {
        global_base = block;
    }

    return block;
}

void split_block(struct block_meta *block, size_t size) {
    if (block->size >= size + META_SIZE + ALIGNMENT) {
        struct block_meta *new_block = (struct block_meta *)((char *)(block + 1) + size);
        new_block->size      = block->size - size - META_SIZE;
        new_block->next      = block->next;
        new_block->prev      = block;
        new_block->next_free = NULL;
        new_block->prev_free = NULL;
        new_block->free      = 1;
        new_block->magic     = 0x55555555;

        if (new_block->next) {
            new_block->next->prev = new_block;
        } else {
            global_last = new_block;
        }

        block->size = size;
        block->next = new_block;

        add_to_free_list(new_block);
    }
}

struct block_meta *merge_block(struct block_meta *block) {
    // 1. Merge with next if it is free
    if (block->next && block->next->free) {
        remove_from_free_list(block->next);
        block->size += META_SIZE + block->next->size;
        block->next = block->next->next;
        if (block->next) {
            block->next->prev = block;
        } else {
            global_last = block;
        }
    }

    // 2. Merge with prev if it is free
    if (block->prev && block->prev->free) {
        remove_from_free_list(block->prev);
        block->prev->size += META_SIZE + block->size;
        block->prev->next = block->next;
        if (block->next) {
            block->next->prev = block->prev;
        } else {
            global_last = block->prev;
        }
        block = block->prev;
    }

    return block;
}

int shrink_heap(struct block_meta *block) {
    // Only shrink if the block is free and is the last block in the list (at the top of the heap)
    if (block && block->free && block->next == NULL) {
        // The physical end address of the block exactly matches the current program break
        if ((char *)(block + 1) + block->size == sbrk(0)) {
            size_t bytes = block->size + META_SIZE;
            if (block->prev) {
                block->prev->next = NULL;
                global_last = block->prev;
            } else {
                global_base = NULL;
                global_last = NULL;
            }
            // return memory to the kernel via system calls
            sbrk(-(intptr_t)bytes);
            return 1;
        }
    }
    return 0;
}

void *malloc(size_t size) {
    if (size <= 0) {
        return NULL;
    }

    size = ALIGN(size);

    struct block_meta *block = find_free_block(size);
    if (!block) {
        block = request_space(size);
        if (!block) {
            return NULL;
        }
    } else {
        remove_from_free_list(block);
        split_block(block, size);
        block->free  = 0;
        block->magic = 0x77777777;
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

    block_ptr = merge_block(block_ptr);
    if (!shrink_heap(block_ptr)) {
        add_to_free_list(block_ptr);
    }
}

void *realloc(void *ptr, size_t size) {
    if (!ptr) {
        // NULL ptr. realloc should act like malloc.
        return malloc(size);
    }

    if (size == 0) {
        free(ptr);
        return NULL;
    }

    size = ALIGN(size);
    struct block_meta *block_ptr = get_block_ptr(ptr);

    // 1. In-place shrink / already large enough
    if (block_ptr->size >= size) {
        split_block(block_ptr, size);
        if (block_ptr->next && block_ptr->next->free) {
            struct block_meta *tail = block_ptr->next;
            remove_from_free_list(tail);
            tail = merge_block(tail);
            if (!shrink_heap(tail)) {
                add_to_free_list(tail);
            }
        }
        return ptr;
    }

    // 2. In-place expansion: check if the right neighbor is free and large enough
    if (block_ptr->next && block_ptr->next->free) {
        size_t combined = block_ptr->size + META_SIZE + block_ptr->next->size;
        if (combined >= size) {
            remove_from_free_list(block_ptr->next);
            block_ptr->size = combined;
            block_ptr->next = block_ptr->next->next;
            if (block_ptr->next) {
                block_ptr->next->prev = block_ptr;
            } else {
                global_last = block_ptr;
            }
            split_block(block_ptr, size);
            return ptr;
        }
    } else if (block_ptr->next == NULL && (char *)(block_ptr + 1) + block_ptr->size == sbrk(0)) {
        // At the top of the heap: expand in-place by moving program break!
        size_t diff = size - block_ptr->size;
        void *request = sbrk(diff);
        if (request != (void *)-1) {
            block_ptr->size = size;
            return ptr;
        }
    }

    // 3. Fallback: out-of-place allocation + copy + free
    void *new_ptr = malloc(size);
    if (!new_ptr) {
        return NULL;  // TODO: set errno on failure.
    }
    memcpy(new_ptr, ptr, block_ptr->size);
    free(ptr);
    return new_ptr;
}
