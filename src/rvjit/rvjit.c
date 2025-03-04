/*
rvjit.c - Retargetable Versatile JIT Compiler
Copyright (C) 2021  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "rvjit.h"
#include "rvjit_emit.h"
#include "utils.h"
#include "vector.h"
#include "atomics.h"
#include "bit_ops.h"
#include "vma_ops.h"

#if defined(RVJIT_ARM64) && defined(__APPLE__) && __ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ >= 110000
/*
 * Apple Silicon needs special care with JIT memory protection
 */
#include <pthread.h>
#define RVJIT_APPLE_SILICON
void sys_icache_invalidate(void* start, size_t len);
#endif

#if defined(RVJIT_RISCV) && defined(__linux__)
/*
 * Clang doesn't seem to implement __clear_cache() properly on RISC-V,
 * wreaking havok on hosts with non-coherent icache, just use a syscall
 */
#include <sys/syscall.h>
#include <unistd.h>

/*
 * RISC-V currently has a "global icache flush" scheme so coalescing is preferred
 */
#define RVJIT_GLOBAL_ICACHE_FLUSH

#elif defined(RVJIT_ARM64) && defined(GNU_EXTS)
/*
 * Don't rely on GCC's __clear_cache implementation, as it may
 * use incorrect cacheline sizes on buggy big.LITTLE hardware.
 * TODO: Figure out proper cacheline from the kernel somehow?
 */
static inline void rvjit_arm64_fluch_icache(const void* addr, size_t size)
{
    size_t dsize = 64, isize = 64;
    size_t end = ((size_t)addr) + size;

    // Drain data cache
    for (size_t cl = align_size_down((size_t)addr, dsize); cl < end; cl += dsize) {
        // Use "dc civac" instead of "dc cvau", as this is the suggested workaround for
        // Cortex-A53 errata 819472, 826319, 827319 and 824069.
        __asm__ volatile ("dc civac, %0" : : "r" (cl) : "memory");
    }
    // Store barrier
    __asm__ volatile ("dsb ish" : : : "memory");
    // Flush instruction cache
    for (size_t cl = align_size_down((size_t)addr, isize); cl < end; cl += isize) {
        __asm__ volatile ("ic ivau, %0" : : "r" (cl) : "memory");
    }
    // Load/store barrier
    __asm__ volatile ("dsb ish" : : : "memory");
    __asm__ volatile ("isb" : : : "memory");
}

#elif defined(_WIN32) && !defined(RVJIT_X86) && !defined(GNU_EXTS)
/*
 * FlushInstructionCache() might be used
 */
#include <windows.h>

#endif

static void rvjit_flush_icache(const void* addr, size_t size)
{
#ifdef RVJIT_X86
    // x86 has coherent instruction caches
    UNUSED(addr); UNUSED(size);
#elif defined(RVJIT_ARM64) && defined(GNU_EXTS)
    rvjit_arm64_fluch_icache(addr, size);
#elif defined(RVJIT_APPLE_SILICON)
    sys_icache_invalidate((void*)addr, size);
#elif defined(RVJIT_RISCV) && defined(__linux__) && defined(__NR_riscv_flush_icache)
    syscall(__NR_riscv_flush_icache, addr, ((char*)addr) + size, 0);
#elif GCC_CHECK_VER(4, 7) || CLANG_CHECK_VER(3, 5)
    __builtin___clear_cache((char*)addr, ((char*)addr) + size);
#elif defined(GNU_EXTS)
    // Use legacy __clear_cache() on old GNU compilers
    __clear_cache((char*)addr, ((char*)addr) + size);
#elif defined(_WIN32)
    // This is probably MSVC on ARM
    FlushInstructionCache(GetCurrentProcess(), addr, size);
#else
    #error No rvjit_flush_icache() support!
#endif
}

bool rvjit_ctx_init(rvjit_block_t* block, size_t size)
{
    // Assume it's already inited
    if (block->heap.data) return true;

    if (rvvm_has_arg("rvjit_disable_rwx")) {
        rvvm_info("RWX disabled, allocating W^X multi-mmap RVJIT heap");
    } else {
        block->heap.data = vma_alloc(NULL, size, VMA_RWX);
        block->heap.code = block->heap.data;

        // Possible on Linux PaX (hardened) or OpenBSD
        if (block->heap.data == NULL) {
            rvvm_info("Failed to allocate RWX RVJIT heap, falling back to W^X multi-mmap");
        }
    }

    if (block->heap.data == NULL) {
        void* rw = NULL;
        void* exec = NULL;
        if (!vma_multi_mmap(&rw, &exec, size)) {
            rvvm_warn("Failed to allocate W^X RVJIT heap!");
            return false;
        }
        block->heap.data = rw;
        block->heap.code = exec;
    }

    block->space = 1024;
    block->code = safe_malloc(block->space);

    block->heap.size = size;
    block->heap.curr = 0;

    block->rv64 = false;

    hashmap_init(&block->heap.blocks, 64);
    hashmap_init(&block->heap.block_links, 64);
    vector_init(block->links);
    return true;
}

void rvjit_init_memtracking(rvjit_block_t* block, size_t size)
{
    // Each dirty page is marked in atomic bitmask
    free(block->heap.dirty_pages);
    free(block->heap.jited_pages);
    block->heap.dirty_mask = bit_next_pow2((size + 0x1FFFF) >> 17) - 1;
    block->heap.dirty_pages = safe_new_arr(uint32_t, block->heap.dirty_mask + 1);
    block->heap.jited_pages = safe_new_arr(uint32_t, block->heap.dirty_mask + 1);
}

static void rvjit_linker_cleanup(rvjit_block_t* block)
{
    vector_t(void*)* linked_blocks;
    hashmap_foreach(&block->heap.block_links, k, v) {
        UNUSED(k);
        linked_blocks = (void*)v;
        vector_free(*linked_blocks);
        free(linked_blocks);
    }
    hashmap_clear(&block->heap.block_links);
}

void rvjit_ctx_free(rvjit_block_t* block)
{
    vma_free(block->heap.data, block->heap.size);
    if (block->heap.code != block->heap.data) {
        vma_free((void*)block->heap.code, block->heap.size);
    }
    rvjit_linker_cleanup(block);
    hashmap_destroy(&block->heap.blocks);
    hashmap_destroy(&block->heap.block_links);
    vector_free(block->links);
    free(block->code);
    free(block->heap.dirty_pages);
    free(block->heap.jited_pages);
}

static inline void rvjit_mark_jited_page(rvjit_block_t* block, phys_addr_t addr)
{
    if (block->heap.jited_pages == NULL) return;
    size_t offset = (addr >> 17) & block->heap.dirty_mask;
    uint32_t mask = 1U << ((addr >> 12) & 0x1F);
    atomic_or_uint32_ex(block->heap.jited_pages + offset, mask, ATOMIC_RELAXED);
}

static inline void rvjit_mark_dirty_page(rvjit_block_t* block, phys_addr_t addr)
{
    size_t offset = (addr >> 17) & block->heap.dirty_mask;
    uint32_t mask = 1U << ((addr >> 12) & 0x1F);
    if (atomic_load_uint32_ex(block->heap.jited_pages + offset, ATOMIC_RELAXED) & mask) {
        atomic_or_uint32_ex(block->heap.dirty_pages + offset, mask, ATOMIC_RELAXED);
        atomic_and_uint32_ex(block->heap.jited_pages + offset, ~mask, ATOMIC_RELAXED);
    }
}

void rvjit_mark_dirty_mem(rvjit_block_t* block, phys_addr_t addr, size_t size)
{
    if (block->heap.dirty_pages == NULL) return;
    for (size_t i=0; i<size; i += 4096) {
        rvjit_mark_dirty_page(block, addr + i);
    }
}

static inline bool rvjit_page_needs_flush(rvjit_block_t* block, phys_addr_t addr)
{
    size_t offset = (addr >> 17) & block->heap.dirty_mask;
    uint32_t mask = 1U << ((addr >> 12) & 0x1F);
    if (block->heap.dirty_pages == NULL) return false;
    return (atomic_load_uint32_ex(block->heap.dirty_pages + offset, ATOMIC_RELAXED) & mask)
        && (atomic_and_uint32(block->heap.dirty_pages + offset, ~mask) & mask);
}

void rvjit_block_init(rvjit_block_t* block)
{
    block->size = 0;
    block->linkage = LINKAGE_JMP;
    vector_clear(block->links);
    rvjit_emit_init(block);
}

rvjit_func_t rvjit_block_finalize(rvjit_block_t* block)
{
    void* dest = block->heap.data + block->heap.curr;
    const void* code = block->heap.code + block->heap.curr;

    rvjit_emit_end(block, block->linkage);

    if (block->heap.curr + block->size > block->heap.size) {
        // The cache is full
        return NULL;
    }

#ifdef RVJIT_APPLE_SILICON
    pthread_jit_write_protect_np(false);
#endif

    memcpy(dest, block->code, block->size);
    block->heap.curr += block->size;

    hashmap_put(&block->heap.blocks, block->phys_pc, (size_t)code);

#ifdef RVJIT_NATIVE_LINKER
    vector_t(uint8_t*)* linked_blocks = NULL;
    vector_foreach(block->links, i) {
        phys_addr_t k = vector_at(block->links, i).dest;
        size_t v = vector_at(block->links, i).ptr;
        linked_blocks = (void*)hashmap_get(&block->heap.block_links, k);
        if (!linked_blocks) {
            linked_blocks = safe_calloc(1, sizeof(vector_t(uint8_t*)));
            vector_init(*linked_blocks);
            hashmap_put(&block->heap.block_links, k, (size_t)linked_blocks);
        }
        vector_push_back(*linked_blocks, (uint8_t*)v);
    }

    linked_blocks = (void*)hashmap_get(&block->heap.block_links, block->phys_pc);
    if (linked_blocks) {
        vector_foreach(*linked_blocks, i) {
            uint8_t* jptr = vector_at(*linked_blocks, i);
            rvjit_linker_patch_jmp(jptr, ((size_t)dest) - ((size_t)jptr));
#ifndef RVJIT_GLOBAL_ICACHE_FLUSH
            rvjit_flush_icache(jptr, 8);
#endif
        }
        vector_free(*linked_blocks);
        free(linked_blocks);
        hashmap_remove(&block->heap.block_links, block->phys_pc);
    }
#endif

    rvjit_flush_icache(code, block->size);

#ifdef RVJIT_APPLE_SILICON
    pthread_jit_write_protect_np(true);
#endif

    rvjit_mark_jited_page(block, block->phys_pc);

    return (rvjit_func_t)code;
}

rvjit_func_t rvjit_block_lookup(rvjit_block_t* block, phys_addr_t phys_pc)
{
    if (unlikely(rvjit_page_needs_flush(block, phys_pc))) {
        vector_t(uint8_t*)* linked_blocks;
        phys_pc &= ~0xFFFULL;

        for (size_t i=0; i<4096; ++i) {
            hashmap_remove(&block->heap.blocks, phys_pc + i);
            linked_blocks = (void*)hashmap_get(&block->heap.block_links, phys_pc + i);
            if (linked_blocks) {
                vector_free(*linked_blocks);
                free(linked_blocks);
                hashmap_remove(&block->heap.block_links, phys_pc + i);
            }
        }
        return NULL;
    }
    return (rvjit_func_t)hashmap_get(&block->heap.blocks, phys_pc);
}

void rvjit_flush_cache(rvjit_block_t* block)
{
    if (block->heap.curr > 0x10000) {
        // Deallocate the physical memory used for RWX JIT cache
        // This reduces average memory usage since the cache is never full
        vma_clean(block->heap.data, block->heap.size, true);
    }

    hashmap_clear(&block->heap.blocks);
    block->heap.curr = 0;

    rvjit_linker_cleanup(block);

    if (block->heap.dirty_pages) {
        for (size_t i=0; i<=block->heap.dirty_mask; ++i) {
            atomic_store_uint32_ex(block->heap.dirty_pages + i, 0, ATOMIC_RELAXED);
        }
    }

    rvjit_block_init(block);
}
