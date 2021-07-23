/*
spinlock.h - Atomic spinlock
Copyright (C) 2021  LekKit <github.com/LekKit>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#ifndef SPINLOCK_H
#define SPINLOCK_H

#include "atomics.h"

typedef struct {
    uint32_t flag;
} spinlock_t;

static inline void spin_init(spinlock_t* lock)
{
    lock->flag = 0;
}

static inline void spin_lock(spinlock_t* lock)
{
    while (atomic_swap_uint32(&lock->flag, 1));
}

static inline void spin_unlock(spinlock_t* lock)
{
    atomic_store_uint32(&lock->flag, 0);
}

#endif
