// local
#include "shm.h"

// C/C++ standard library
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

// glibc
#include <errno.h>

// Linux
#include <sys/shm.h>

namespace shm {

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#define PAGE_SIZE 4096
#define ROUND_UP_TO_PAGE(x) ((((x) - 1) & -PAGE_SIZE) + PAGE_SIZE)

static const uint32_t max_capacity =
    INT32_MAX;  // we want to subtract between uint32 offset and get signed
                // result without overflow
static const size_t num_record_size_byte = 2;
static const size_t max_record_size = SHM_MAX_WRITE_SIZE;
static const size_t default_shm_size = 16384 * PAGE_SIZE;  // 64 MiB

static uint64_t bytes_behind(Position write_pos, Position read_pos,
                             uint32_t capacity) {
    int64_t delta_generation =
        static_cast<int64_t>(write_pos.generation) -
        static_cast<int64_t>(read_pos.generation);
    int64_t delta_offset = static_cast<int64_t>(write_pos.offset) -
                           static_cast<int64_t>(read_pos.offset);
    int64_t delta = delta_generation * static_cast<int64_t>(capacity) +
                    delta_offset;
    return delta > 0 ? static_cast<uint64_t>(delta) : 0;
}

/*******************************************************************************
 * Position
 ******************************************************************************/

Position::Position() : opaque(0) {}
Position::Position(const volatile Position &src)
    : opaque(src.opaque) { /* FIXME: atomic load */ }
Position &Position::operator=(const volatile Position &src) {
    opaque = src.opaque;
    return *this;
}
bool Position::operator==(const volatile Position &rhs) {
    return opaque == rhs.opaque;
}

/*******************************************************************************
 * Writer
 ******************************************************************************/

Writer::Writer(key_t key, size_t shm_size_hint) {
    // check argument (shm_size_hint)
    // shm_size_hint is only a hint, not guaranteed
    size_t capacity;
    if (shm_size_hint == 0) {
        shm_size_hint = default_shm_size;
    }
    capacity = ROUND_UP_TO_PAGE(shm_size_hint) - sizeof(Region);
    if (capacity < max_record_size * 2) {
        capacity = max_record_size * 2;
    } else if (capacity > max_capacity) {
        capacity = max_capacity;
    }

    // get or create shm id
    bool created = false;
    size_t size = sizeof(Region) + capacity;
    int id = shmget(key, size, 0);
    if (id == -1) {
        if (errno != ENOENT) {
            log_write(stderr, "%s: shmget(open): %s\n", __PRETTY_FUNCTION__,
                    strerror(errno));
            exit(1);
        }
        id = shmget(key, size, IPC_CREAT | IPC_EXCL | SHM_NORESERVE | 0644);
        if (id == -1) {
            log_write(stderr, "%s: shmget(create): %s\n", __PRETTY_FUNCTION__,
                    strerror(errno));
            exit(1);
        }
        created = true;
    }

    // attach shm into address space
    void *addr = shmat(id, NULL, 0);
    if (addr == (void *)-1) {
        log_write(stderr, "%s: shmat: %s\n", __PRETTY_FUNCTION__,
                strerror(errno));
        exit(1);
    }

    // IPC_RMID does not work because key was reset to 0
    // Let's rely on kernel.shm_rmid_forced=1

    // ensure shm is locked in RAM
    if (created) {
        if (shmctl(id, SHM_LOCK, NULL) == -1) {
            // CAP_IPC_LOCK & RLIMIT_MEMLOCK
            log_write(stderr, "%s: shmctl(SHM_LOCK): %s\n", __PRETTY_FUNCTION__,
                    strerror(errno));
            exit(1);
        }
        // shmctl(2): The caller must fault in any pages that are required to be
        // present after locking is enabled.
        char *end =
            (char *)addr +
            size;  // no overflow would occur due to address space layout
        for (char *page = (char *)addr; page < end; page += PAGE_SIZE) {
            *(volatile long *)page;
        }
    }

    // initialize (create) or check (open)
    Region *region = (Region *)addr;
    if (created) {
        region->capacity = capacity;  // setting capacity to non-zero marks that
                                      // Region was initialized
    } else {
        if (region->capacity != capacity) {
            log_write(stderr, "%s: capacity mismatch\n", __PRETTY_FUNCTION__);
            exit(1);
        }
        if (region->write_position.offset >= capacity) {
            log_write(stderr, "%s: invalid write_position\n",
                    __PRETTY_FUNCTION__);
            exit(1);
        }
    }

    m_region = region;
}

Writer::Writer(Writer &&other) : m_region(other.m_region) {
    other.m_region = nullptr;
}

Writer::~Writer() {
    if (m_region) {
        log_write(stderr, "%s: shmdt\n", __PRETTY_FUNCTION__);
        shmdt((void *)m_region);
    }
}

void Writer::write(const void *data, size_t size) {
    write_impl(data, size, false, 0);
}

void Writer::write_with_stamp_patch(const void *data, size_t size,
                                    size_t stamp_offset) {
    write_impl(data, size, true, stamp_offset);
}

void Writer::write_impl(const void *data, size_t size, bool patch_stamp,
                        size_t stamp_offset) {
    if (unlikely(size <= 0 || size > max_record_size)) {
        static bool prompted = false;
        if (!prompted) {
            prompted = true;
            log_write(stderr, "%s: invalid record size, record dropped\n",
                    __PRETTY_FUNCTION__);
        }
        return;
    }
    if (unlikely(patch_stamp && stamp_offset + sizeof(uint64_t) > size)) {
        static bool prompted = false;
        if (!prompted) {
            prompted = true;
            log_write(stderr, "%s: invalid stamp patch offset, record dropped\n",
                    __PRETTY_FUNCTION__);
        }
        return;
    }

    uint32_t capacity = m_region->capacity;
    Position w = m_region->write_position;
    m_region->data[w.offset++] = size & 0xFF;
    if (unlikely(w.offset >= capacity)) {
        w.offset = 0;
        ++w.generation;
    }
    m_region->data[w.offset++] = size >> 8;
    uint32_t payload_offset = w.offset;
    uint32_t remain = capacity - w.offset;
    if (likely(size < remain)) {
        memcpy(&m_region->data[w.offset], data, size);
        w.offset += size;
    } else {
        memcpy(&m_region->data[w.offset], data, remain);
        ++w.generation;
        w.offset = size - remain;
        memcpy(&m_region->data[0], (const char *)data + remain, w.offset);
    }

    if (patch_stamp) {
        const uint64_t stamp = fast_tsc::now();
        uint32_t patch_offset = payload_offset + static_cast<uint32_t>(stamp_offset);
        if (patch_offset >= capacity) patch_offset -= capacity;

        uint32_t patch_remain = capacity - patch_offset;
        if (likely(sizeof(stamp) <= patch_remain)) {
            memcpy(&m_region->data[patch_offset], &stamp, sizeof(stamp));
        } else {
            memcpy(&m_region->data[patch_offset], &stamp, patch_remain);
            memcpy(&m_region->data[0],
                   reinterpret_cast<const uint8_t*>(&stamp) + patch_remain,
                   sizeof(stamp) - patch_remain);
        }
    }

    m_region->write_position = w;  // FIXME: atomic store
}

/*******************************************************************************
 * Reader
 ******************************************************************************/

Reader::Reader() : m_region(nullptr), m_size(0), m_error(not_opened_yet) {
    void *buffer = malloc(max_record_size);
    if (buffer == NULL) {
        log_write(stderr, "%s: unable to allocate buffer\n", __PRETTY_FUNCTION__);
        exit(1);
    }
    m_buffer = (uint8_t *)buffer;
}

Reader::Reader(Reader &&other)
    : m_region(other.m_region),
      m_read_position(other.m_read_position),
      m_buffer(other.m_buffer),
      m_size(other.m_size),
      m_error(other.m_error) {
    other.m_region = nullptr;
    other.m_buffer = nullptr;
}

Reader::~Reader() {
    if (m_buffer) {
        free(m_buffer);
    }
    if (m_region) {
        log_write(stderr, "%s: shmdt\n", __PRETTY_FUNCTION__);
        shmdt((void *)m_region);
    }
}

void Reader::open(key_t key) {
    int id = shmget(key, 0, 0);
    if (id == -1) {
        if (errno == ENOENT) {
            // writer does not exist at present
            return;
        }
        log_write(stderr, "%s: shmget: %s\n", __PRETTY_FUNCTION__,
                strerror(errno));
        exit(1);
    }
    void *addr = shmat(id, NULL, SHM_RDONLY);
    if (addr == (void *)-1) {
        log_write(stderr, "%s: shmat: %s\n", __PRETTY_FUNCTION__,
                strerror(errno));
        exit(1);
    }
    volatile Region *region = (volatile Region *)addr;
    uint32_t capacity;
    while (!(capacity = region->capacity)) {
    }  // spin, wait writer set capacity
    if (capacity < max_record_size * 2 || capacity > max_capacity) {
        log_write(stderr, "%s: invalid capacity\n", __PRETTY_FUNCTION__);
        exit(1);
    }
    m_region = region;
    m_read_position = region->write_position;
    m_error = readable;
}

void Reader::reset() {
    if (likely(m_error == read_left_behind)) {
        m_read_position = m_region->write_position;
        m_size = 0;
        m_error = readable;
    }
}

ReaderStatus Reader::check_position(size_t payload_size) {
    uint32_t capacity = m_region->capacity;
    Position w = m_region->write_position;
    Position &r = m_read_position;
    if (unlikely(w.offset >= capacity || r.offset >= capacity)) {
        return invalid_state;
    }
    uint32_t record_size = payload_size + num_record_size_byte;
    int32_t delta_generation = w.generation - r.generation;
    int32_t delta_offset = w.offset - r.offset;
    if (likely(delta_generation == 0)) {
        if (likely(delta_offset == 0)) {
            return nothing_to_read;
        }
        if (unlikely(delta_offset <= 1)) {
            return invalid_state;
        }
        if (unlikely(delta_offset > (int32_t)(capacity - record_size))) {
            return read_left_behind;
        }
        return readable;
    }
    if (likely(delta_generation == 1)) {
        if (unlikely(-delta_offset > (int32_t)(capacity - record_size))) {
            return invalid_state;
        } else if (unlikely(-delta_offset < (int32_t)record_size)) {
            return read_left_behind;
        }
        return readable;
    }
    if (unlikely(delta_generation < 0)) {
        return invalid_state;
    } else /* delta_generation > 1 */ {
        return read_left_behind;
    }
}

// only called when there is something to read (no matter good or corrupted)
ReaderStatus Reader::read_one() {
    bool next_generation = false;
    uint32_t capacity = m_region->capacity;
    volatile uint8_t *data = m_region->data;
    uint32_t offset = m_read_position.offset;
    // prefetch next record
    uint32_t size = data[offset++];
    if (unlikely(offset >= capacity)) {
        offset = 0;
        next_generation = true;
    }
    size |= uint32_t(data[offset++]) << 8;
    if (size == 0 || size > max_record_size) {
        // check if caused by read_left_behind (recoverable after reset)
        ReaderStatus status = check_position(max_record_size);
        if (likely(status == read_left_behind)) {
            return read_left_behind;
        } else {
            return invalid_state;
        }
    }
    // size may still be invalid
    uint32_t remain = capacity - offset;
    if (likely(size < remain)) {
        memcpy(m_buffer, (void *)&m_region->data[offset], size);
        offset += size;
    } else {
        memcpy(m_buffer, (void *)&m_region->data[offset], remain);
        offset = size - remain;
        next_generation = true;
        memcpy(&m_buffer[remain], (void *)&m_region->data[0], offset);
    }
    ReaderStatus status = check_position(size);
    if (likely(status == readable)) {
        m_size = size;
        m_read_position.offset = offset;
        if (next_generation) {
            ++m_read_position.generation;
        }
        return readable;
    } else {
        m_size = 0;
        return status;
    }
}

ReaderStatus Reader::read() {
    if (unlikely(m_error)) {
        return m_error;
    }
    if (m_read_position == m_region->write_position) {
        return nothing_to_read;
    }
    ReaderStatus status = read_one();
    if (unlikely(status < 0)) {
        if (unlikely(status == nothing_to_read)) {
            status = invalid_state;
        }
        if (unlikely(status == read_left_behind)) {
            Position write_pos = m_region->write_position;
            uint32_t capacity = m_region->capacity;
            log_write(stderr,
                      "shm::Reader: ring buffer overrun, reader left behind "
                      "(capacity=%uB behind=%lluB write_gen=%u write_off=%u "
                      "read_gen=%u read_off=%u)\n",
                      capacity,
                      (unsigned long long)bytes_behind(write_pos,
                                                       m_read_position,
                                                       capacity),
                      write_pos.generation, write_pos.offset,
                      m_read_position.generation, m_read_position.offset);
        }
        m_error = status;
        return status;
    }
    return readable;
}

}  // namespace shm
