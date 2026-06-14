#include <chrono>
#include <cstdio>
#include <iostream>
#include <thread>
#include <vector>

#include "shm.h"

using namespace std::chrono;

struct RecordHeader {
    timespec timestamp;
    uint32_t key;
    uint32_t size;
} __attribute__((packed));

static const size_t MAX_RECORD_SIZE = 4096;
static const size_t LARGE_BUFFER_SIZE = 256 * 1024 * 1024;
static const double REPLAY_SPEED = 0.01;

constexpr nanoseconds to_duration(timespec ts) {
    return seconds{ts.tv_sec} + nanoseconds{ts.tv_nsec};
}

int main(int argc, const char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <record-file>\n", argv[0]);
        return 1;
    }
    FILE *fp = fopen(argv[1], "rb");
    if (fp == nullptr) {
        fprintf(stderr, "failed to open record file\n");
        abort();
    }
    static const uint32_t keys[]{
        0xFEED0001,
        0xC0DE0001,
    };
    static const size_t num_keys = sizeof(keys) / sizeof(keys[0]);
    shm::Writer writers[num_keys]{
        {key_t(keys[0]), LARGE_BUFFER_SIZE},
        {key_t(keys[1]), LARGE_BUFFER_SIZE},
    };
    timespec data_begin = {0, 0};
    time_point steady_begin = steady_clock::now();
    while (true) {
        RecordHeader hdr;
        char body[MAX_RECORD_SIZE] = {0};
        size_t n = fread(&hdr, sizeof hdr, 1, fp);
        if (n == 0) {
            break;
        }

        if (hdr.size > MAX_RECORD_SIZE) {
            fprintf(stderr,
                    "failed to read record: record is too large, size = %u",
                    hdr.size);
            abort();
        }
        n = fread(body, hdr.size, 1, fp);
        if (n != 1) {
            fprintf(stderr, "failed to read record: read body");
            abort();
        }
        if (data_begin.tv_sec == 0) {
            data_begin = hdr.timestamp;
        }

        auto data_elapsed =
            to_duration(hdr.timestamp) - to_duration(data_begin);
        auto data_elapsed_scaled = data_elapsed * REPLAY_SPEED;
        auto steady_elapsed = steady_clock::now() - steady_begin;
        if (data_elapsed_scaled > steady_elapsed) {
            std::this_thread::sleep_for(data_elapsed_scaled - steady_elapsed);
        }
        for (size_t i = 0; i < num_keys; ++i) {
            if (hdr.key == keys[i]) {
                writers[i].write(body, hdr.size);
            }
        }
    }
    fclose(fp);
}
