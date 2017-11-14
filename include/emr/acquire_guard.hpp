#pragma once

namespace emr {

template <typename ConcurrentPtr>
auto acquire_guard(ConcurrentPtr& p, std::memory_order order = std::memory_order_seq_cst)
{
  typename ConcurrentPtr::guard_ptr guard;
  guard.acquire(p, order);
  return guard;
}

}