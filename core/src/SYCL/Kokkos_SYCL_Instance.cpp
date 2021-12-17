/*
//@HEADER
// ************************************************************************
//
//                        Kokkos v. 3.0
//       Copyright (2020) National Technology & Engineering
//               Solutions of Sandia, LLC (NTESS).
//
// Under the terms of Contract DE-NA0003525 with NTESS,
// the U.S. Government retains certain rights in this software.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
// 1. Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright
// notice, this list of conditions and the following disclaimer in the
// documentation and/or other materials provided with the distribution.
//
// 3. Neither the name of the Corporation nor the names of the
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY NTESS "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL NTESS OR THE
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
// LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// Questions? Contact Christian R. Trott (crtrott@sandia.gov)
//
// ************************************************************************
//@HEADER
*/

#include <Kokkos_Core.hpp>  //kokkos_malloc

namespace Kokkos {
namespace Experimental {
namespace Impl {

std::vector<std::optional<sycl::queue>*> SYCLInternal::all_queues;
std::mutex SYCLInternal::mutex;

SYCLInternal::~SYCLInternal() {
  if (!was_finalized || m_scratchSpace || m_scratchFlags ||
      m_scratchConcurrentBitset) {
    std::cerr << "Kokkos::Experimental::SYCL ERROR: Failed to call "
                 "Kokkos::Experimental::SYCL::finalize()"
              << std::endl;
    std::cerr.flush();
  }
}

int SYCLInternal::verify_is_initialized(const char* const label) const {
  if (!is_initialized()) {
    std::cerr << "Kokkos::Experimental::SYCL::" << label
              << " : ERROR device not initialized" << std::endl;
  }
  return is_initialized();
}
SYCLInternal& SYCLInternal::singleton() {
  static SYCLInternal self;
  return self;
}

void SYCLInternal::initialize(const sycl::device& d) {
  auto exception_handler = [](sycl::exception_list exceptions) {
    bool asynchronous_error = false;
    for (std::exception_ptr const& e : exceptions) {
      try {
        std::rethrow_exception(e);
      } catch (sycl::exception const& e) {
        std::cerr << e.what() << '\n';
        asynchronous_error = true;
      }
    }
    if (asynchronous_error)
      Kokkos::Impl::throw_runtime_exception(
          "There was an asynchronous SYCL error!\n");
  };
  // FIXME_SYCL using an in-order queue here should not be necessary since we
  // are using submit_barrier for managing kernel dependencies but this seems to
  // be required as a hot fix for now.
  initialize(
      sycl::queue{d, exception_handler, sycl::property::queue::in_order()});
}

// FIXME_SYCL
void SYCLInternal::initialize(const sycl::queue& q) {
  if (was_finalized)
    Kokkos::abort("Calling SYCL::initialize after SYCL::finalize is illegal\n");

  if (is_initialized()) return;

  if (!HostSpace::execution_space::impl_is_initialized()) {
    const std::string msg(
        "SYCL::initialize ERROR : HostSpace::execution_space is not "
        "initialized");
    Kokkos::Impl::throw_runtime_exception(msg);
  }

  const bool ok_init = nullptr == m_scratchSpace || nullptr == m_scratchFlags;
  const bool ok_dev  = true;
  if (ok_init && ok_dev) {
    m_queue = q;
    // guard pushing to all_queues
    {
      std::scoped_lock lock(mutex);
      all_queues.push_back(&m_queue);
    }
    const sycl::device& d = m_queue->get_device();

    m_maxWorkgroupSize =
        d.template get_info<sycl::info::device::max_work_group_size>();
    // FIXME_SYCL this should give the correct value for NVIDIA GPUs
    m_maxConcurrency =
        m_maxWorkgroupSize * 2 *
        d.template get_info<sycl::info::device::max_compute_units>();

    // Setup concurent bitset for obtaining unique tokens from within an
    // executing kernel.
    {
      const int32_t buffer_bound =
          Kokkos::Impl::concurrent_bitset::buffer_bound(m_maxConcurrency);
      using Record = Kokkos::Impl::SharedAllocationRecord<
          Kokkos::Experimental::SYCLDeviceUSMSpace, void>;
      Record* const r =
          Record::allocate(Kokkos::Experimental::SYCLDeviceUSMSpace(*m_queue),
                           "Kokkos::Experimental::SYCL::InternalScratchBitset",
                           sizeof(uint32_t) * buffer_bound);
      Record::increment(r);
      m_scratchConcurrentBitset = reinterpret_cast<uint32_t*>(r->data());
      auto event                = m_queue->memset(m_scratchConcurrentBitset, 0,
                                   sizeof(uint32_t) * buffer_bound);
      fence(event,
            "Kokkos::Experimental::SYCLInternal::initialize: fence after "
            "initializing m_scratchConcurrentBitset",
            m_instance_id);
    }

    m_maxShmemPerBlock =
        d.template get_info<sycl::info::device::local_mem_size>();

    m_indirectReducerMem.reset(*m_queue, m_instance_id);
    for (auto& usm_mem : m_indirectKernelMem) {
      usm_mem.reset(*m_queue, m_instance_id);
    }

  } else {
    std::ostringstream msg;
    msg << "Kokkos::Experimental::SYCL::initialize(...) FAILED";

    if (!ok_init) {
      msg << " : Already initialized";
    }
    Kokkos::Impl::throw_runtime_exception(msg.str());
  }

  m_team_scratch_current_size = 0;
  m_team_scratch_ptr          = nullptr;
}

void* SYCLInternal::resize_team_scratch_space(std::int64_t bytes,
                                              bool force_shrink) {
  if (m_team_scratch_current_size == 0) {
    m_team_scratch_current_size = bytes;
    m_team_scratch_ptr =
        Kokkos::kokkos_malloc<Experimental::SYCLDeviceUSMSpace>(
            "Kokkos::Experimental::SYCLDeviceUSMSpace::TeamScratchMemory",
            m_team_scratch_current_size);
  }
  if ((bytes > m_team_scratch_current_size) ||
      ((bytes < m_team_scratch_current_size) && (force_shrink))) {
    m_team_scratch_current_size = bytes;
    m_team_scratch_ptr =
        Kokkos::kokkos_realloc<Experimental::SYCLDeviceUSMSpace>(
            m_team_scratch_ptr, m_team_scratch_current_size);
  }
  return m_team_scratch_ptr;
}

uint32_t SYCLInternal::impl_get_instance_id() const { return m_instance_id; }

void SYCLInternal::finalize() {
  SYCLInternal::fence(*m_queue,
                      "Kokkos::SYCLInternal::finalize: fence on finalization",
                      m_instance_id);
  was_finalized = true;

  using RecordSYCL = Kokkos::Impl::SharedAllocationRecord<SYCLDeviceUSMSpace>;
  if (nullptr != m_scratchSpace)
    RecordSYCL::decrement(RecordSYCL::get_record(m_scratchSpace));
  if (nullptr != m_scratchFlags)
    RecordSYCL::decrement(RecordSYCL::get_record(m_scratchFlags));
  m_syclDev           = -1;
  m_scratchSpaceCount = 0;
  m_scratchSpace      = nullptr;
  m_scratchFlagsCount = 0;
  m_scratchFlags      = nullptr;

  RecordSYCL::decrement(RecordSYCL::get_record(m_scratchConcurrentBitset));
  m_scratchConcurrentBitset = nullptr;

  if (m_team_scratch_current_size > 0)
    Kokkos::kokkos_free<Kokkos::Experimental::SYCLDeviceUSMSpace>(
        m_team_scratch_ptr);
  m_team_scratch_current_size = 0;
  m_team_scratch_ptr          = nullptr;

  for (auto& usm_mem : m_indirectKernelMem) usm_mem.reset();
  m_indirectReducerMem.reset();
  // guard erasing from all_queues
  {
    std::scoped_lock lock(mutex);
    all_queues.erase(std::find(all_queues.begin(), all_queues.end(), &m_queue));
  }
  m_queue.reset();
}

void* SYCLInternal::scratch_space(const std::size_t size) {
  const size_type sizeScratchGrain =
      sizeof(Kokkos::Experimental::SYCL::size_type);
  if (verify_is_initialized("scratch_space") &&
      m_scratchSpaceCount * sizeScratchGrain < size) {
    m_scratchSpaceCount = (size + sizeScratchGrain - 1) / sizeScratchGrain;

    using Record = Kokkos::Impl::SharedAllocationRecord<
        Kokkos::Experimental::SYCLDeviceUSMSpace, void>;

    if (nullptr != m_scratchSpace)
      Record::decrement(Record::get_record(m_scratchSpace));

    Record* const r =
        Record::allocate(Kokkos::Experimental::SYCLDeviceUSMSpace(*m_queue),
                         "Kokkos::Experimental::SYCL::InternalScratchSpace",
                         (sizeScratchGrain * m_scratchSpaceCount));

    Record::increment(r);

    m_scratchSpace = reinterpret_cast<size_type*>(r->data());
  }

  return m_scratchSpace;
}

void* SYCLInternal::scratch_flags(const std::size_t size) {
  const size_type sizeScratchGrain =
      sizeof(Kokkos::Experimental::SYCL::size_type);
  if (verify_is_initialized("scratch_flags") &&
      m_scratchFlagsCount * sizeScratchGrain < size) {
    m_scratchFlagsCount = (size + sizeScratchGrain - 1) / sizeScratchGrain;

    using Record = Kokkos::Impl::SharedAllocationRecord<
        Kokkos::Experimental::SYCLDeviceUSMSpace, void>;

    if (nullptr != m_scratchFlags)
      Record::decrement(Record::get_record(m_scratchFlags));

    Record* const r =
        Record::allocate(Kokkos::Experimental::SYCLDeviceUSMSpace(*m_queue),
                         "Kokkos::Experimental::SYCL::InternalScratchFlags",
                         (sizeScratchGrain * m_scratchFlagsCount));

    Record::increment(r);

    m_scratchFlags = reinterpret_cast<size_type*>(r->data());
  }
  m_queue->memset(m_scratchFlags, 0, m_scratchFlagsCount * sizeScratchGrain);
  fence(*m_queue,
        "Kokkos::Experimental::SYCLInternal::scratch_flags fence after "
        "initializing m_scratchFlags",
        m_instance_id);

  return m_scratchFlags;
}

template <typename WAT>
void SYCLInternal::fence_helper(WAT& wat, const std::string& name,
                                uint32_t instance_id) {
  Kokkos::Tools::Experimental::Impl::profile_fence_event<
      Kokkos::Experimental::SYCL>(
      name, Kokkos::Tools::Experimental::Impl::DirectFenceIDHandle{instance_id},
      [&]() {
        try {
          wat.wait_and_throw();
        } catch (sycl::exception const& e) {
          Kokkos::Impl::throw_runtime_exception(
              std::string("There was a synchronous SYCL error:\n") += e.what());
        }
      });
}
template void SYCLInternal::fence_helper<sycl::queue>(sycl::queue&,
                                                      const std::string&,
                                                      uint32_t);
template void SYCLInternal::fence_helper<sycl::event>(sycl::event&,
                                                      const std::string&,
                                                      uint32_t);

// This function cycles through a pool of USM allocations for functors
SYCLInternal::IndirectKernelMem& SYCLInternal::get_indirect_kernel_mem() {
  /* Thread safety:
     Is it sufficient to atomically increment m_pool_next here?
     Suppose m_usm_pool_size is 2, and we have 3 threads attempting to grab
     USMObjectMem. Threads 0 and 2 will get the same resource. Is this OK?
     `copy_from` is mutex locked anyway, and semantically, it's OK for two
     threads to want to use the same USMObjectMem, so long as one ends up having
     to wait for m_last_event, so it should be safe.
  */
  size_t next_pool = desul::atomic_wrapping_fetch_inc(
      &m_pool_next, m_usm_pool_size - 1, desul::MemoryOrderRelaxed(),
      desul::MemoryScopeDevice());
  return m_indirectKernelMem[next_pool];
}

template <sycl::usm::alloc Kind>
size_t SYCLInternal::USMObjectMem<Kind>::reserve(size_t n) {
  assert(m_q);

  if (m_capacity < n) {
    using Record = Kokkos::Impl::SharedAllocationRecord<AllocationSpace, void>;
    // First free what we have (in case malloc can reuse it)
    if (m_data) Record::decrement(Record::get_record(m_data));

    Record* const r = Record::allocate(
        AllocationSpace(*m_q), "Kokkos::Experimental::SYCL::USMObjectMem", n);
    Record::increment(r);

    m_data = r->data();
    m_staging.reset(new char[n]);
    m_capacity = n;
  }

  return m_capacity;
}

template <sycl::usm::alloc Kind>
void SYCLInternal::USMObjectMem<Kind>::reset() {
  if (m_data) {
    // This implies a fence since this class is not copyable
    // and deallocating implies a fence across all registered queues.
    using Record = Kokkos::Impl::SharedAllocationRecord<AllocationSpace, void>;
    Record::decrement(Record::get_record(m_data));

    m_capacity = 0;
    m_data     = nullptr;
  }
  m_q.reset();
}

template class SYCLInternal::USMObjectMem<sycl::usm::alloc::shared>;
template class SYCLInternal::USMObjectMem<sycl::usm::alloc::device>;
template class SYCLInternal::USMObjectMem<sycl::usm::alloc::host>;

}  // namespace Impl
}  // namespace Experimental
}  // namespace Kokkos
