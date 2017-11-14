#ifndef EPOCH_BASED_IMPL
#error "This is an impl file and must not be included directly!"
#endif

#include "emr/detail/orphan.hpp"

#include <algorithm>

namespace emr {

  template <std::size_t UpdateThreshold>
  template <class T, class MarkedPtr>
  epoch_based<UpdateThreshold>::guard_ptr<T, MarkedPtr>::guard_ptr(const MarkedPtr& p) noexcept :
    base(p)
  {
    if (this->ptr)
      local_thread_data().enter_critical();
  }

  template <std::size_t UpdateThreshold>
  template <class T, class MarkedPtr>
  epoch_based<UpdateThreshold>::guard_ptr<T, MarkedPtr>::guard_ptr(const guard_ptr& p) noexcept :
    guard_ptr(MarkedPtr(p))
  {}

  template <std::size_t UpdateThreshold>
  template <class T, class MarkedPtr>
  epoch_based<UpdateThreshold>::guard_ptr<T, MarkedPtr>::guard_ptr(guard_ptr&& p) noexcept :
    base(p.ptr)
  {
    p.ptr.reset();
  }

  template <std::size_t UpdateThreshold>
  template <class T, class MarkedPtr>
  auto epoch_based<UpdateThreshold>::guard_ptr<T, MarkedPtr>::operator=(const guard_ptr& p) noexcept
    -> guard_ptr&
  {
    if (&p == this)
      return *this;

    reset();
    this->ptr = p.ptr;
    if (this->ptr)
      local_thread_data().enter_critical();

    return *this;
  }

  template <std::size_t UpdateThreshold>
  template <class T, class MarkedPtr>
  auto epoch_based<UpdateThreshold>::guard_ptr<T, MarkedPtr>::operator=(guard_ptr&& p) noexcept
    -> guard_ptr&
  {
    if (&p == this)
      return *this;

    reset();
    this->ptr = std::move(p.ptr);
    p.ptr.reset();

    return *this;
  }

  template <std::size_t UpdateThreshold>
  template <class T, class MarkedPtr>
  void epoch_based<UpdateThreshold>::guard_ptr<T, MarkedPtr>::acquire(concurrent_ptr<T>& p,
    std::memory_order order) noexcept
  {
    if (p.load(std::memory_order_relaxed) == nullptr)
    {
      reset();
      return;
    }

    if (!this->ptr)
      local_thread_data().enter_critical();
    // (1) - this load operation potentially synchronizes-with any release operation on p.
    this->ptr = p.load(order);
    if (!this->ptr)
      local_thread_data().leave_critical();
  }

  template <std::size_t UpdateThreshold>
  template <class T, class MarkedPtr>
  bool epoch_based<UpdateThreshold>::guard_ptr<T, MarkedPtr>::acquire_if_equal(
    concurrent_ptr<T>& p,
    const MarkedPtr& expected,
    std::memory_order order) noexcept
  {
    auto actual = p.load(std::memory_order_relaxed);
    if (actual == nullptr || actual != expected)
    {
      reset();
      return actual == expected;
    }

    if (!this->ptr)
      local_thread_data().enter_critical();
    // (2) - this load operation potentially synchronizes-with any release operation on p.
    this->ptr = p.load(order);
    if (!this->ptr || this->ptr != expected)
    {
      local_thread_data().leave_critical();
      this->ptr.reset();
    }

    return this->ptr == expected;
  }

  template <std::size_t UpdateThreshold>
  template <class T, class MarkedPtr>
  void epoch_based<UpdateThreshold>::guard_ptr<T, MarkedPtr>::reset() noexcept
  {
    if (this->ptr)
      local_thread_data().leave_critical();
    this->ptr.reset();
  }

  template <std::size_t UpdateThreshold>
  template <class T, class MarkedPtr>
  void epoch_based<UpdateThreshold>::guard_ptr<T, MarkedPtr>::reclaim(Deleter d) noexcept
  {
    this->ptr->set_deleter(std::move(d));
    local_thread_data().add_retired_node(this->ptr.get());
    reset();
  }

  template <std::size_t UpdateThreshold>
  struct epoch_based<UpdateThreshold>::thread_control_block :
    detail::thread_block_list<thread_control_block>::entry
  {
    thread_control_block() :
      is_in_critical_region(false),
      local_epoch(number_epochs)
    {}

    std::atomic<bool> is_in_critical_region;
    std::atomic<unsigned> local_epoch;
  };

  template <std::size_t UpdateThreshold>
  struct epoch_based<UpdateThreshold>::thread_data
  {
    ~thread_data()
    {
      if (control_block == nullptr)
        return; // no control_block -> nothing to do

      // we can avoid creating an orphan in case we have no retired nodes left.
      if (std::any_of(retire_lists.begin(), retire_lists.end(), [](auto p) { return p != nullptr; }))
      {
        // global_epoch - 1 (mod number_epochs) guarantees a full cycle, making sure no
        // other thread may still have a reference to an object in one of the retire lists.
        auto target_epoch = (global_epoch.load(std::memory_order_relaxed) + number_epochs - 1) % number_epochs;
        assert(target_epoch < number_epochs);
        global_thread_block_list.abandon_retired_nodes(new detail::orphan<number_epochs>(target_epoch, retire_lists));
      }

      assert(control_block->is_in_critical_region.load(std::memory_order_relaxed) == false);
      global_thread_block_list.release_entry(control_block);
    }

    void enter_critical()
    {
      if (++enter_count == 1)
        do_enter_critical();
    }

    void leave_critical()
    {
      assert(enter_count > 0);
      if (--enter_count == 0)
        do_leave_critical();
    }

    void add_retired_node(detail::deletable_object* p)
    {
      add_retired_node(p, control_block->local_epoch.load(std::memory_order_relaxed));
    }

  private:
    void ensure_has_control_block()
    {
      if (control_block == nullptr)
        control_block = global_thread_block_list.acquire_entry();
    }

    void do_enter_critical()
    {
      ensure_has_control_block();

      control_block->is_in_critical_region.store(true, std::memory_order_relaxed);
      // (3) - this seq_cst-fence enforces a total order with itself
      std::atomic_thread_fence(std::memory_order_seq_cst);

      // (4) - this acquire-load synchronizes-with the release-CAS (7)
      auto epoch = global_epoch.load(std::memory_order_acquire);
      if (control_block->local_epoch.load(std::memory_order_relaxed) != epoch) // New epoch?
      {
        entries_since_update = 0;
      }
      else if (entries_since_update++ == UpdateThreshold)
      {
        entries_since_update = 0;
        const auto new_epoch = (epoch + 1) % number_epochs;
        if (!try_update_epoch(epoch, new_epoch))
          return;

        epoch = new_epoch;
      }
      else
        return;

      // we either just updated the global_epoch or we are observing a new epoch from some other thread
      // either way - we can reclaim all the objects from the old 'incarnation' of this epoch

      control_block->local_epoch.store(epoch, std::memory_order_relaxed);
      detail::delete_objects(retire_lists[epoch]);
    }

    void do_leave_critical()
    {
      // (5) - this release-store synchronizes-with the acquire-fence (6)
      control_block->is_in_critical_region.store(false, std::memory_order_release);
    }

    void add_retired_node(detail::deletable_object* p, size_t epoch)
    {
      assert(epoch < number_epochs);
      p->next = retire_lists[epoch];
      retire_lists[epoch] = p;
    }

    bool try_update_epoch(unsigned curr_epoch, unsigned new_epoch)
    {
      const auto old_epoch = (curr_epoch + number_epochs - 1) % number_epochs;
      auto prevents_update = [old_epoch](const thread_control_block& data)
      {
        return data.is_in_critical_region.load(std::memory_order_relaxed) &&
               data.local_epoch.load(std::memory_order_relaxed) == old_epoch;
      };

      // If any thread hasn't advanced to the current epoch, abort the attempt.
      auto can_update = !std::any_of(global_thread_block_list.begin(), global_thread_block_list.end(),
                                     prevents_update);
      if (!can_update)
        return false;

      if (global_epoch.load(std::memory_order_relaxed) == curr_epoch)
      {
        // (6) - this acquire-fence synchronizes-with the release-store (5)
        std::atomic_thread_fence(std::memory_order_acquire);

        // (7) - this release-CAS synchronizes-with the acquire-load (4)
        bool success = global_epoch.compare_exchange_strong(curr_epoch, new_epoch,
                                                            std::memory_order_release,
                                                            std::memory_order_relaxed);
        if (success)
          adopt_orphans();
      }

      // return true regardless of whether the CAS operation was successful or not
      // it's not import that THIS thread updated the epoch, but it got updated in any case
      return true;
    }

    void adopt_orphans()
    {
      auto cur = global_thread_block_list.adopt_abandoned_retired_nodes();
      for (detail::deletable_object* next = nullptr; cur != nullptr; cur = next)
      {
        next = cur->next;
        cur->next = nullptr;
        add_retired_node(cur, static_cast<detail::orphan<number_epochs>*>(cur)->target_epoch);
      }
    }

    unsigned enter_count = 0;
    unsigned entries_since_update = 0;
    thread_control_block* control_block = nullptr;
    std::array<detail::deletable_object*, number_epochs> retire_lists = {};

    friend class epoch_based;
    ALLOCATION_COUNTER(epoch_based);
  };

  template <std::size_t UpdateThreshold>
  std::atomic<unsigned> epoch_based<UpdateThreshold>::global_epoch;

  template <std::size_t UpdateThreshold>
  detail::thread_block_list<typename epoch_based<UpdateThreshold>::thread_control_block>
    epoch_based<UpdateThreshold>::global_thread_block_list;

  template <std::size_t UpdateThreshold>
  inline typename epoch_based<UpdateThreshold>::thread_data& epoch_based<UpdateThreshold>::local_thread_data()
  {
    static thread_local thread_data local_thread_data;
    return local_thread_data;
  }

#ifdef TRACK_ALLOCATIONS
  template <std::size_t UpdateThreshold>
  emr::detail::allocation_tracker epoch_based<UpdateThreshold>::allocation_tracker;

  template <std::size_t UpdateThreshold>
  inline void epoch_based<UpdateThreshold>::count_allocation()
  { local_thread_data().allocation_counter.count_allocation(); }

  template <std::size_t UpdateThreshold>
  inline void epoch_based<UpdateThreshold>::count_reclamation()
  { local_thread_data().allocation_counter.count_reclamation(); }
#endif
}
