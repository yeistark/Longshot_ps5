/* (PS5) What the console SDK's C++ library does not have and the renderer uses, so gpu/vk stays as upstream writes it.
   ps5/build.py includes this ahead of every C++ unit of gpu/vk. */
#ifndef HOS_PS5_CXX_COMPAT_H
#define HOS_PS5_CXX_COMPAT_H
#ifdef __cplusplus
#include <atomic>
#include <version>
#include <spawn.h>
/// glibc's "close every descriptor from this one up" for a spawned program (gpu/vk/vk_menu.cpp starts the desktop's browser
/// with it). The console starts no programs: nothing to add, and the spawn itself is refused there
static inline int posix_spawn_file_actions_addclosefrom_np(posix_spawn_file_actions_t*, int) { return 0; }
#ifndef __cpp_lib_atomic_ref
namespace std {
/// std::atomic_ref (C++20), over the compiler's atomic builtins: the object itself is the storage, as the standard's is
template <class T> class atomic_ref {
    T* p_;
    static constexpr int order(memory_order o) {
        return o == memory_order_relaxed ? __ATOMIC_RELAXED : o == memory_order_acquire ? __ATOMIC_ACQUIRE
             : o == memory_order_release ? __ATOMIC_RELEASE : o == memory_order_acq_rel ? __ATOMIC_ACQ_REL
             : o == memory_order_consume ? __ATOMIC_CONSUME : __ATOMIC_SEQ_CST;
    }
    static constexpr int fail_order(memory_order o) {
        return o == memory_order_acq_rel ? __ATOMIC_ACQUIRE : o == memory_order_release ? __ATOMIC_RELAXED : order(o);
    }
public:
    using value_type = T;
    explicit atomic_ref(T& v) noexcept : p_(&v) {}
    atomic_ref(const atomic_ref&) noexcept = default;
    T load(memory_order o = memory_order_seq_cst) const noexcept { T v; __atomic_load(p_, &v, order(o)); return v; }
    void store(T v, memory_order o = memory_order_seq_cst) const noexcept { __atomic_store(p_, &v, order(o)); }
    T exchange(T v, memory_order o = memory_order_seq_cst) const noexcept { T r; __atomic_exchange(p_, &v, &r, order(o)); return r; }
    bool compare_exchange_strong(T& expected, T desired, memory_order o = memory_order_seq_cst) const noexcept {
        return __atomic_compare_exchange(p_, &expected, &desired, false, order(o), fail_order(o));
    }
    bool compare_exchange_strong(T& expected, T desired, memory_order ok, memory_order fail) const noexcept {
        return __atomic_compare_exchange(p_, &expected, &desired, false, order(ok), order(fail));
    }
    bool compare_exchange_weak(T& expected, T desired, memory_order o = memory_order_seq_cst) const noexcept {
        return __atomic_compare_exchange(p_, &expected, &desired, true, order(o), fail_order(o));
    }
    bool compare_exchange_weak(T& expected, T desired, memory_order ok, memory_order fail) const noexcept {
        return __atomic_compare_exchange(p_, &expected, &desired, true, order(ok), order(fail));
    }
    T fetch_add(T v, memory_order o = memory_order_seq_cst) const noexcept { return __atomic_fetch_add(p_, v, order(o)); }
    T fetch_sub(T v, memory_order o = memory_order_seq_cst) const noexcept { return __atomic_fetch_sub(p_, v, order(o)); }
    T fetch_or(T v, memory_order o = memory_order_seq_cst) const noexcept { return __atomic_fetch_or(p_, v, order(o)); }
    T fetch_and(T v, memory_order o = memory_order_seq_cst) const noexcept { return __atomic_fetch_and(p_, v, order(o)); }
    operator T() const noexcept { return load(); }
    T operator=(T v) const noexcept { store(v); return v; }
};
}   // namespace std
#endif
#endif
#endif
