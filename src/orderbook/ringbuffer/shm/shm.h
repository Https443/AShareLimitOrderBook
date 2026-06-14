#pragma once

// C/C++ standard library
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

// Linux
#include <sys/shm.h>

// sanity check
#if SIZE_MAX < UINT32_MAX
#error antique?
#endif

namespace shm {

static const size_t SHM_MAX_WRITE_SIZE = 4096;

union Position {
    uint64_t opaque;
    struct {
        uint32_t offset;
        uint32_t generation;
    };

    Position();
    Position(const volatile Position&);
    Position& operator=(const volatile Position&);
    bool operator==(const volatile Position&);
};

struct Region {
    Position write_position;
    uint32_t capacity;
    uint8_t data[];
};

class Writer {
    Region* m_region;

   public:
    Writer(key_t key, size_t shm_size_hint = 0);
    Writer(Writer&& other);
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;
    ~Writer();
    void write(const void* data, size_t size);
    // Patch a 64-bit stamp into the payload after the copy completes and
    // before write_position is published. stamp_offset is relative to the
    // payload start, not the 2-byte record-size header.
    void write_with_stamp_patch(const void* data, size_t size,
                                size_t stamp_offset);

   private:
    void write_impl(const void* data, size_t size, bool patch_stamp,
                    size_t stamp_offset);
};

enum ReaderStatus : int32_t {
    readable = 0,
    nothing_to_read = -1,   // reader at same position as writer
    read_left_behind = -2,  // read too slow or write too fast
    invalid_state = -3,     // logic bug or memory corruption
    not_opened_yet = -4,    // didn't call Reader::open()
};

class Reader {
    volatile Region* m_region;
    Position m_read_position;
    uint8_t* m_buffer;
    uint32_t m_size;
    ReaderStatus m_error;

   public:
    Reader();
    Reader(Reader&& other);
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;
    ~Reader();
    void open(key_t key);
    void reset();
    ReaderStatus read();
    const void* get_buffer() { return (void*)m_buffer; }
    size_t get_size() { return m_size; }

   private:
    ReaderStatus check_position(size_t payload_size);
    ReaderStatus read_one();
};

}  // namespace shm
