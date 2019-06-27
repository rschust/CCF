// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the Apache 2.0 License.
#include <condition_variable>
#include <stdint.h>

#define OPEN_ENCLAVE
#define snmalloc snmalloc_shared

#ifndef IS_ADDRESS_SPACE_CONSTRAINED
#  define IS_ADDRESS_SPACE_CONSTRAINED
#endif

#include "pal_open_enclave_untrusted.h"

namespace snmalloc
{
  class Superslab;
  class Mediumslab;
}

namespace
{
  struct SharedPageMap
  {
    uint8_t get(uintptr_t p);
    void set_slab(snmalloc::Superslab* slab);
    void clear_slab(snmalloc::Superslab* slab);
    void clear_slab(snmalloc::Mediumslab* slab);
    void set_slab(snmalloc::Mediumslab* slab);
    void set_large_size(void* p, size_t size);
    void clear_large_size(void* p, size_t size);

  private:
    void set(void* p, uint8_t x);
  };
}

#define SNMALLOC_DEFAULT_PAGEMAP SharedPageMap
#define SNMALLOC_MEMORY_PROVIDER PALOpenEnclaveUntrusted

#define SNMALLOC_NAME_MANGLE(a) enclave_shared_##a
#define NO_BOOTSTRAP_ALLOCATOR
// Redefine the namespace, so we can have two versions.
#include <override/malloc.cc>

SuperslabPagemap* _pagemap = &global_pagemap;

extern "C" void set_ccf_shared_pagemap(void* pm)
{
  _pagemap = (SuperslabPagemap*)pm;
}

namespace
{
  /**
   * Get the pagemap entry corresponding to a specific address.
   */
  uint8_t SharedPageMap::get(uintptr_t p)
  {
    return _pagemap->get(p);
  }
  /**
   * Set a pagemap entry indicating that there is a superslab at the
   * specified index.
   */
  void SharedPageMap::set_slab(Superslab* slab)
  {
    set(slab, (size_t)PMSuperslab);
  }
  /**
   * Add a pagemap entry indicating that a medium slab has been allocated.
   */
  void SharedPageMap::set_slab(Mediumslab* slab)
  {
    set(slab, (size_t)PMMediumslab);
  }
  /**
   * Remove an entry from the pagemap corresponding to a superslab.
   */
  void SharedPageMap::clear_slab(Superslab* slab)
  {
    assert(get(slab) == PMSuperslab);
    set(slab, (size_t)PMNotOurs);
  }
  /**
   * Remove an entry corresponding to a medium slab.
   */
  void SharedPageMap::clear_slab(Mediumslab* slab)
  {
    assert(get(slab) == PMMediumslab);
    set(slab, (size_t)PMNotOurs);
  }
  /**
   * Update the pagemap to reflect a large allocation, of `size` bytes from
   * address `p`.
   */
  void SharedPageMap::set_large_size(void* p, size_t size)
  {
    size_t size_bits = bits::next_pow2_bits(size);
    set(p, (uint8_t)size_bits);
    // Set redirect slide
    uintptr_t ss = (uintptr_t)((size_t)p + SUPERSLAB_SIZE);
    for (size_t i = 0; i < size_bits - SUPERSLAB_BITS; i++)
    {
      size_t run = 1ULL << i;
      _pagemap->set_range(ss, (uint8_t)(64 + i + SUPERSLAB_BITS), run);
      ss = ss + SUPERSLAB_SIZE * run;
    }
    _pagemap->set(address_cast(p), (uint8_t)size_bits);
  }
  /**
   * Update the pagemap to remove a large allocation, of `size` bytes from
   * address `p`.
   */
  void SharedPageMap::clear_large_size(void* p, size_t size)
  {
    size_t rounded_size = bits::next_pow2(size);
    assert(get(p) == bits::next_pow2_bits(size));
    auto count = rounded_size >> SUPERSLAB_BITS;
    _pagemap->set_range(address_cast(p), PMNotOurs, count);
  }

  /**
   * Helper function to set a pagemap entry.  This is not part of the public
   * interface and exists to make it easy to reuse the code in the public
   * methods in other pagemap adaptors.
   */
  void SharedPageMap::set(void* p, uint8_t x)
  {
    _pagemap->set(address_cast(p), x);
  }
}

#undef OPEN_ENCLAVE