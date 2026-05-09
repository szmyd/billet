#pragma once

#include <array>
#include <cstddef>
#include <vector>

namespace billet::engine {

class aligned_buf_pool;

// Owns one aligned heap allocation. Returns to its pool on destruction. Move-only.
class aligned_buf {
public:
    aligned_buf() noexcept = default;
    aligned_buf(aligned_buf const&) = delete;
    aligned_buf& operator=(aligned_buf const&) = delete;
    aligned_buf(aligned_buf&& other) noexcept;
    aligned_buf& operator=(aligned_buf&& other) noexcept;
    ~aligned_buf();

    void* data() noexcept { return _data; }
    void const* data() const noexcept { return _data; }
    size_t size() const noexcept { return _size; }
    explicit operator bool() const noexcept { return nullptr != _data; }

private:
    friend class aligned_buf_pool;
    aligned_buf(aligned_buf_pool* pool, void* data, size_t size) noexcept : _pool(pool), _data(data), _size(size) {}

    aligned_buf_pool* _pool{nullptr};
    void* _data{nullptr};
    size_t _size{0};
};

// Slab pool of buffers aligned to a fixed runtime alignment (the device physical
// block size). Buffers are bucketed into a small fixed set of size classes; each
// acquire() rounds up to the smallest containing class.
//
// Thread-safety: NOT thread-safe by design. Each worker thread owns its own pool,
// eliminating cross-thread contention on the I/O hot path.
class aligned_buf_pool {
public:
    static constexpr std::array< size_t, 4 > k_size_classes{
        size_t{4} * 1024,
        size_t{8} * 1024,
        size_t{64} * 1024,
        size_t{512} * 1024,
    };

    explicit aligned_buf_pool(size_t alignment);
    ~aligned_buf_pool();

    aligned_buf_pool(aligned_buf_pool const&) = delete;
    aligned_buf_pool& operator=(aligned_buf_pool const&) = delete;

    // Acquire a buffer of at least `size` bytes. Throws std::bad_alloc if `size`
    // exceeds the largest class or posix_memalign fails.
    aligned_buf acquire(size_t size);

    // Pre-allocate `count` buffers in the size class containing `size`. Used at
    // startup to fail-fast on OOM rather than mid-run.
    void reserve(size_t size, size_t count);

    size_t alignment() const noexcept { return _alignment; }

private:
    friend class aligned_buf;
    void release(void* p, size_t size_class) noexcept;

    // Returns the index in k_size_classes whose class contains `size`, or
    // k_size_classes.size() if `size` exceeds the largest class.
    static size_t classify(size_t size) noexcept;

    void* allocate_one(size_t size_class);

    size_t _alignment;
    std::array< std::vector< void* >, k_size_classes.size() > _free;
};

} // namespace billet::engine
