#include <engine/aligned_buf.hpp>

#include <bit>
#include <cstdlib>
#include <new>
#include <stdexcept>

namespace billet::engine {

aligned_buf::aligned_buf(aligned_buf&& other) noexcept : _pool(other._pool), _data(other._data), _size(other._size) {
    other._pool = nullptr;
    other._data = nullptr;
    other._size = 0;
}

aligned_buf& aligned_buf::operator=(aligned_buf&& other) noexcept {
    if (this != &other) {
        if (nullptr != _pool && nullptr != _data) { _pool->release(_data, _size); }
        _pool = other._pool;
        _data = other._data;
        _size = other._size;
        other._pool = nullptr;
        other._data = nullptr;
        other._size = 0;
    }
    return *this;
}

aligned_buf::~aligned_buf() {
    if (nullptr != _pool && nullptr != _data) { _pool->release(_data, _size); }
}

aligned_buf_pool::aligned_buf_pool(size_t alignment) : _alignment(alignment) {
    if (0 == alignment || !std::has_single_bit(alignment)) {
        throw std::invalid_argument("aligned_buf_pool: alignment must be a power of two");
    }
    if (alignment < sizeof(void*)) {
        throw std::invalid_argument("aligned_buf_pool: alignment must be >= sizeof(void*)");
    }
}

aligned_buf_pool::~aligned_buf_pool() {
    for (auto& bucket : _free) {
        for (void* p : bucket) {
            std::free(p);
        }
    }
}

size_t aligned_buf_pool::classify(size_t size) noexcept {
    for (size_t i = 0; k_size_classes.size() > i; ++i) {
        if (k_size_classes[i] >= size) { return i; }
    }
    return k_size_classes.size();
}

void* aligned_buf_pool::allocate_one(size_t size_class) {
    void* p = nullptr;
    int const rc = ::posix_memalign(&p, _alignment, size_class);
    if (0 != rc) { throw std::bad_alloc(); }
    return p;
}

aligned_buf aligned_buf_pool::acquire(size_t size) {
    auto const idx = classify(size);
    if (k_size_classes.size() == idx) { throw std::bad_alloc(); }

    auto& bucket = _free[idx];
    if (!bucket.empty()) {
        void* p = bucket.back();
        bucket.pop_back();
        return aligned_buf(this, p, k_size_classes[idx]);
    }
    return aligned_buf(this, allocate_one(k_size_classes[idx]), k_size_classes[idx]);
}

void aligned_buf_pool::reserve(size_t size, size_t count) {
    auto const idx = classify(size);
    if (k_size_classes.size() == idx) { throw std::bad_alloc(); }

    auto& bucket = _free[idx];
    bucket.reserve(bucket.size() + count);
    for (size_t i = 0; count > i; ++i) {
        bucket.push_back(allocate_one(k_size_classes[idx]));
    }
}

void aligned_buf_pool::release(void* p, size_t size_class) noexcept {
    auto const idx = classify(size_class);
    if (k_size_classes.size() == idx) {
        // Shouldn't happen since acquire() rejects oversize; free directly to avoid leak.
        std::free(p);
        return;
    }
    try {
        _free[idx].push_back(p);
    } catch (...) {
        // Freelist growth failed; fall back to free() so we never leak.
        std::free(p);
    }
}

} // namespace billet::engine
