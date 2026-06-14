#include <signal.h>
#include <time.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <thread>
#include <vector>

#include "shm.h"

static FILE *fp;
static bool need_flush;

void on_record(uint32_t key, const void *data, size_t size) {
    // Note: CLOCK_REALTIME may experience discontinuities and backwards jumps
    // caused by NTP inserting leap seconds.
    struct timespec t = {0, 0};
    clock_gettime(CLOCK_REALTIME, &t);
    if (fwrite(&t, sizeof t, 1, fp) != 1) goto err;
    uint32_t hdr[2];
    hdr[0] = key;
    hdr[1] = (uint32_t)size;
    if (fwrite(&hdr, sizeof hdr, 1, fp) != 1) goto err;
    if (fwrite(data, size, 1, fp) != 1) goto err;
    return;

err:
    fprintf(stderr, "unable to write output file\n");
    abort();
}

void sigalrm_handler(__attribute__((unused)) int signum) {
    alarm(15);
    need_flush = true;
}

const std::chrono::nanoseconds poll_interval = std::chrono::milliseconds(1);

int main(int argc, char **argv) {
    size_t num_channels = 12;
    if (argc > 1) {
        num_channels = atoi(argv[1]);
        if (num_channels == 0) {
            fprintf(stderr, "invalid number of channels: %s\n", argv[1]);
            return 1;
        }
    }

    char filename[32];
    std::time_t time = std::time(nullptr);
    strftime(filename, sizeof filename, "record-%Y%m%d-%H%M%S.journal",
             std::localtime(&time));
    fp = fopen(filename, "wb");
    if (fp == nullptr) {
        fprintf(stderr, "unable to open output file\n");
        abort();
    }

    signal(SIGALRM, sigalrm_handler);
    alarm(15);

    std::vector<uint32_t> keys;
    for (uint32_t i = 0; i < num_channels; ++i) {
        keys.push_back(0xC0DE0000 + (i << 4));
        keys.push_back(0xC0DE0001 + (i << 4));
        keys.push_back(0xFEED0000 + (i << 4));
        keys.push_back(0xFEED0001 + (i << 4));
    }
    for (auto key : keys) {
        fprintf(stderr, "key: 0x%08X\n", key);
    }
    std::vector<shm::Reader> readers(keys.size());
    while (1) {
        auto begin = std::chrono::steady_clock::now();

        if (need_flush) {
            need_flush = false;
            fflush(fp);
        }

        for (size_t i = 0; i < keys.size(); ++i) {
            auto key = keys[i];
            auto &reader = readers[i];
            while (1) {
                shm::ReaderStatus status = reader.read();
                switch (status) {
                    case shm::nothing_to_read:
                        break;
                    case shm::readable:
                        on_record(key, reader.get_buffer(), reader.get_size());
                        continue;
                    case shm::read_left_behind:
                        fprintf(stderr, "[0x%08X] read left behind, reset\n",
                                key);
                        reader.reset();
                        continue;
                    case shm::not_opened_yet:
                        reader.open(key);
                        break;
                    default:
                        fprintf(stderr, "[0x%08X] read fatal error %d\n", key,
                                status);
                        return 1;
                }
                break;
            }
        }

        auto elapsed = std::chrono::steady_clock::now() - begin;
        if (elapsed < poll_interval) {
            std::this_thread::sleep_for(poll_interval - elapsed);
        }
    }
}
