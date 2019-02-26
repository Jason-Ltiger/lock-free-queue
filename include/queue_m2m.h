/*
 * https://coolshell.cn/articles/8239.html
 * http://www.voidcn.com/article/p-sijuqlbv-zs.html
 * http://blog.jobbole.com/107955/
*/

#pragma once

#include <atomic>
#include <new>
#include <utility>
#include <tuple>
#include <cstdint>

namespace m2m {
namespace detail {

template <std::size_t Bytes>
struct tagged_factor;

template <>
struct tagged_factor<4> {
    enum : std::uint64_t {
        mask = 0x00000000fffffffful,
        incr = 0x0000000100000000ul
    };
};

template <>
struct tagged_factor<8> {
    enum : std::uint64_t {
        mask = 0x0000fffffffffffful,
        incr = 0x0001000000000000ul
    };
};

template <typename T, std::size_t Bytes = sizeof(T)>
class tagged {

    enum : std::uint64_t {
        mask = tagged_factor<Bytes>::mask,
        incr = tagged_factor<Bytes>::incr
    };

    std::uint64_t data_ { 0 };

public:
    tagged() = default;
    tagged(tagged const &) = default;

    tagged(T ptr)
        : data_(reinterpret_cast<std::uint64_t>(ptr))
    {}

    tagged(T ptr, std::uint64_t tag)
        : data_(reinterpret_cast<std::uint64_t>(ptr) | ((tag + incr) & ~mask))
    {}

    tagged(std::uint64_t num)
        : data_(num)
    {}

    tagged& operator=(tagged const &) = default;

    std::uint64_t data() const {
        return data_;
    }

    operator T() const {
        return reinterpret_cast<T>(data_ & mask);
    }

    T    operator->() const { return  static_cast<T>(*this); }
    auto operator* () const { return *static_cast<T>(*this); }
};

} // namespace detail

template <typename T>
class tagged {

    std::atomic<std::uint64_t> data_ { 0 };

public:
    tagged() = default;
    tagged(tagged const &) = default;

    tagged(T ptr)
        : data_(reinterpret_cast<std::uint64_t>(ptr))
    {}

    tagged& operator=(tagged const &) = default;

    operator T() const { return load(); }

    T    operator->() const { return  static_cast<T>(*this); }
    auto operator* () const { return *static_cast<T>(*this); }

    auto load(std::memory_order order) {
        return detail::tagged<T>{ data_.load(order) };
    }

    bool compare_exchange_weak(detail::tagged<T>& exp, T val, std::memory_order order) {
        auto num = exp.data();
        if (data_.compare_exchange_weak(num, detail::tagged<T>{ val, num }.data(), order)) {
            return true;
        }
        exp = num;
        return false;
    }
};

template <typename T>
class pool {

    union node {
        T data_;
        std::atomic<node*> next_;
    };

    tagged<node*> cursor_ { nullptr };

public:
    ~pool() {
        auto curr = cursor_.load(std::memory_order_relaxed);
        while (curr != nullptr) {
            auto temp = curr->next_.load(std::memory_order_relaxed);
            delete curr;
            curr = temp;
        }
    }

    bool empty() const {
        return cursor_.load(std::memory_order_acquire) == nullptr;
    }

    template <typename... P>
    T* alloc(P&&... pars) {
        auto curr = cursor_.load(std::memory_order_acquire);
        while (1) {
            if (curr == nullptr) {
                return &((new node { std::forward<P>(pars)... })->data_);
            }
            if (cursor_.compare_exchange_weak(curr, curr->next_, std::memory_order_acquire)) {
                break;
            }
        }
        return ::new (&(curr->data_)) T { std::forward<P>(pars)... };
    }

    void free(void* p) {
        if (p == nullptr) return;
        auto temp = reinterpret_cast<node*>(p);
        auto curr = cursor_.load(std::memory_order_relaxed);
        while (1) {
            temp->next_.store(curr, std::memory_order_relaxed);
            if (cursor_.compare_exchange_weak(curr, temp, std::memory_order_release)) {
                break;
            }
        }
    }
};

template <typename F>
class scope_exit {
    F f_;

public:
    scope_exit(F f) : f_(f) {}
    ~scope_exit() { f_(); }
};

namespace frl {

template <typename T>
class queue {

    struct node {
        T data_;
        std::atomic<node*> next_;
    } dummy_ { {}, nullptr };

    std::atomic<node*> head_ { &dummy_ };
    std::atomic<node*> tail_ { &dummy_ };

    pool<node> allocator_;

    std::atomic<unsigned> counter_   { 0 };
    std::atomic<node*>    free_list_ { nullptr };

    void add_ref() {
        counter_.fetch_add(1, std::memory_order_release);
    }

    void del_ref(node* item) {
        if (item == &dummy_ || item == nullptr) {
            counter_.fetch_sub(1, std::memory_order_release);
            return;
        }
        auto put_free_list = [this](node* first, node* last) {
            auto list = free_list_.load(std::memory_order_relaxed);
            while (1) {
                last->next_.store(list, std::memory_order_relaxed);
                if (free_list_.compare_exchange_weak(list, first, std::memory_order_release)) {
                    break;
                }
            }
        };
        if (counter_.load(std::memory_order_acquire) > 1) {
            put_free_list(item, item);
            counter_.fetch_sub(1, std::memory_order_release);
        }
        else {
            auto temp = free_list_.exchange(nullptr, std::memory_order_acq_rel);
            if (counter_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                while (temp != nullptr) {
                    auto next = temp->next_.load(std::memory_order_relaxed);
                    allocator_.free(temp);
                    temp = next;
                }
            }
            else if (temp != nullptr) {
                // fetch the last
                auto last = temp;
                while (1) {
                    auto next = last->next_.load(std::memory_order_relaxed);
                    if (next == nullptr) {
                        break;
                    }
                    last = next;
                }
                put_free_list(temp, last);
            }
            allocator_.free(item);
        }
    }

public:
    void quit() {}

    bool empty() const {
        return head_.load(std::memory_order_acquire)
             ->next_.load(std::memory_order_relaxed) == nullptr;
    }

    void push(T const & val) {
        auto n = allocator_.alloc(val, nullptr);
        tail_.exchange(n, std::memory_order_relaxed)
         ->next_.store(n, std::memory_order_release);
    }

//    void push(T const & val) {
//        auto n = allocator_.alloc(val, nullptr);
//        while (1) {
//            node* temp = tail_.load(std::memory_order_relaxed);
//            node* next = nullptr;
//            if (temp->next_.compare_exchange_weak(next, n, std::memory_order_relaxed)) {
//                tail_.compare_exchange_strong(temp, n, std::memory_order_release);
//                break;
//            }
//        }
//    }

    std::tuple<T, bool> pop() {
        T ret;
        add_ref();
        auto curr = head_.load(std::memory_order_acquire);
        while (1) {
            node* next = curr->next_.load(std::memory_order_relaxed);
            if (next == nullptr) {
                del_ref(nullptr);
                return {};
            }
            if (head_.compare_exchange_weak(curr, next, std::memory_order_acquire)) {
                ret = next->data_;
                del_ref(curr);
                break;
            }
        }
        return std::make_tuple(ret, true);
    }
};

} // namespace frl
} // namespace m2m
