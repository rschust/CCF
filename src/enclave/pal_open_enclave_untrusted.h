// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the Apache 2.0 License.
#pragma once

#ifdef OPEN_ENCLAVE

#  include <ccf_t.h>
#  include <ds/address.h>
#  include <ds/bits.h>
#  include <pal/pal_consts.h>
#  include <stdio.h>
#  include <strings.h>
#  include <sys/mman.h>

extern "C" oe_result_t host_reserve(void** _retval, size_t size, size_t align);

namespace snmalloc
{
  class PALOpenEnclaveUntrusted
  {
  public:
    /**
     * Bitmap of PalFeatures flags indicating the optional features that this
     * PAL supports.
     */
    static constexpr uint64_t pal_features = AlignedAllocation;
    static void error(const char* const str)
    {
      puts(str);
      abort();
    }

    /// Notify platform that we will not be using these pages
    void notify_not_using(void* p, size_t size) noexcept
    {
      // We do not notify the platform, in order to avoid the OCALL.
      // This will probably also save an OCALL to reserve shared memory
      // at a  later stage, since this memory can be reused for future
      // allocations.
      UNUSED(p);
      UNUSED(size);
    }

    /// Notify platform that we will be using these pages
    template <ZeroMem zero_mem>
    void notify_using(void* p, size_t size) noexcept
    {
      assert(
        bits::is_aligned_block<OS_PAGE_SIZE>(p, size) || (zero_mem == NoZero));
      if constexpr (zero_mem == YesZero)
        zero(p, size);
    }

    /// OS specific function for zeroing memory
    template <bool page_aligned = false>
    void zero(void* p, size_t size) noexcept
    {
      bzero(p, size);
    }

    template <bool committed>
    void* reserve(size_t* size, size_t align) noexcept
    {
      // Alignment must be a power of 2.
      assert(align == bits::next_pow2(align));

      if (align == 0)
      {
        align = 1;
      }

      void* p;
      oe_result_t ocall_res = host_reserve(&p, *size, align);

      if (p == MAP_FAILED)
        error("Out of memory");

      return p;
    }
  };
}

#endif // defined(OPEN_ENCLAVE)
