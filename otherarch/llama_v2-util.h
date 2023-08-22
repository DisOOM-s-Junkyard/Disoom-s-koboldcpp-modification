// Internal header to be included only by llama.cpp.
// Contains wrappers around OS interfaces.
#pragma once
#ifndef LLAMA_V2_UTIL_H
#define LLAMA_V2_UTIL_H

#include "llama-util.h"

#include <cstdio>
#include <cstdint>
#include <cerrno>
#include <cstring>
#include <cstdarg>
#include <cstdlib>
#include <climits>

#include <string>
#include <vector>
#include <stdexcept>

#ifdef __has_include
    #if __has_include(<unistd.h>)
        #include <unistd.h>
        #if defined(_POSIX_MAPPED_FILES)
            #include <sys/mman.h>
        #endif
        #if defined(_POSIX_MEMLOCK_RANGE)
            #include <sys/resource.h>
        #endif
    #endif
#endif

#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
    #include <io.h>
    #include <stdio.h> // for _fseeki64
#endif

#define LLAMA_V2_ASSERT(x) \
    do { \
        if (!(x)) { \
            fprintf(stderr, "LLAMA_V2_ASSERT: %s:%d: %s\n", __FILE__, __LINE__, #x); \
            abort(); \
        } \
    } while (0)

#ifdef __GNUC__
#ifdef __MINGW32__
__attribute__((format_old(gnu_printf, 1, 2)))
#else
__attribute__((format_old(printf, 1, 2)))
#endif
#endif


struct llama_v2_file {
    // use FILE * so we don't have to re-open the file to mmap
    FILE * fp;
    size_t size;

    llama_v2_file(const char * fname, const char * mode) {
        fp = std::fopen(fname, mode);
        if (fp == NULL) {
            throw std::runtime_error(format_old("failed to open %s: %s", fname, strerror(errno)));
        }
        seek(0, SEEK_END);
        size = tell();
        seek(0, SEEK_SET);
    }

    size_t tell() const {
#ifdef _WIN32
        __int64 ret = _ftelli64(fp);
#else
        long ret = std::ftell(fp);
#endif
        LLAMA_V2_ASSERT(ret != -1); // this really shouldn't fail
        return (size_t) ret;
    }

    void seek(size_t offset, int whence) {
#ifdef _WIN32
        int ret = _fseeki64(fp, (__int64) offset, whence);
#else
        int ret = std::fseek(fp, (long) offset, whence);
#endif
        LLAMA_V2_ASSERT(ret == 0); // same
    }

    void read_raw(void * ptr, size_t size) {
        if (size == 0) {
            return;
        }
        errno = 0;
        std::size_t ret = std::fread(ptr, size, 1, fp);
        if (ferror(fp)) {
            throw std::runtime_error(format_old("read error: %s", strerror(errno)));
        }
        if (ret != 1) {
            throw std::runtime_error(std::string("unexpectedly reached end of file"));
        }
    }

    std::uint32_t read_u32() {
        std::uint32_t ret;
        read_raw(&ret, sizeof(ret));
        return ret;
    }

    std::string read_string(std::uint32_t len) {
        std::vector<char> chars(len);
        read_raw(chars.data(), len);
        return std::string(chars.data(), len);
    }

    void write_raw(const void * ptr, size_t size) {
        if (size == 0) {
            return;
        }
        errno = 0;
        size_t ret = std::fwrite(ptr, size, 1, fp);
        if (ret != 1) {
            throw std::runtime_error(format_old("write error: %s", strerror(errno)));
        }
    }

    void write_u32(std::uint32_t val) {
        write_raw(&val, sizeof(val));
    }

    ~llama_v2_file() {
        if (fp) {
            std::fclose(fp);
        }
    }
};

#if defined(_WIN32)
static std::string llama_v2_format_win_err(DWORD err) {
    LPSTR buf;
    size_t size = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                 NULL, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&buf, 0, NULL);
    if (!size) {
        return "FormatMessageA failed";
    }
    std::string ret(buf, size);
    LocalFree(buf);
    return ret;
}
#endif

struct llama_v2_mmap {
    void * addr;
    size_t size;

    llama_v2_mmap(const llama_v2_mmap &) = delete;

#ifdef _POSIX_MAPPED_FILES
    static constexpr bool SUPPORTED = true;

    llama_v2_mmap(struct llama_v2_file * file, bool prefetch = true) {
        size = file->size;
        int fd = fileno(file->fp);
        int flags = MAP_SHARED;
#ifdef __linux__
        flags |= MAP_POPULATE;
#endif
        addr = mmap(NULL, file->size, PROT_READ, flags, fd, 0);
        if (addr == MAP_FAILED) {
            throw std::runtime_error(format("mmap failed: %s", strerror(errno)));
        }

        if (prefetch) {
            // Advise the kernel to preload the mapped memory
            if (madvise(addr, file->size, MADV_WILLNEED)) {
                fprintf(stderr, "warning: madvise(.., MADV_WILLNEED) failed: %s\n",
                        strerror(errno));
            }
        }
    }

    ~llama_v2_mmap() {
        munmap(addr, size);
    }
#elif defined(_WIN32)
    static constexpr bool SUPPORTED = true;

    llama_v2_mmap(struct llama_v2_file * file, bool prefetch = true) {
        size = file->size;

        HANDLE hFile = (HANDLE) _get_osfhandle(_fileno(file->fp));

        HANDLE hMapping = CreateFileMappingA(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
        DWORD error = GetLastError();

        if (hMapping == NULL) {
            throw std::runtime_error(format_old("CreateFileMappingA failed: %s", llama_v2_format_win_err(error).c_str()));
        }

        addr = MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, 0);
        error = GetLastError();
        CloseHandle(hMapping);

        if (addr == NULL) {
            throw std::runtime_error(format_old("MapViewOfFile failed: %s", llama_v2_format_win_err(error).c_str()));
        }

        #ifndef USE_FAILSAFE
        #if _WIN32_WINNT >= _WIN32_WINNT_WIN8
        if (prefetch) {
            // Advise the kernel to preload the mapped memory
            WIN32_MEMORY_RANGE_ENTRY range;
            range.VirtualAddress = addr;
            range.NumberOfBytes = (SIZE_T)size;
            if (!PrefetchVirtualMemory(GetCurrentProcess(), 1, &range, 0)) {
                fprintf(stderr, "warning: PrefetchVirtualMemory failed: %s\n",
                        llama_v2_format_win_err(GetLastError()).c_str());
            }
        }
        #else
        #pragma message("warning: You are building for pre-Windows 8; prefetch not supported")
        #endif // _WIN32_WINNT >= _WIN32_WINNT_WIN8
        #else
        printf("\nPrefetchVirtualMemory skipped in compatibility mode.\n");
        #endif
    }

    ~llama_v2_mmap() {
        if (!UnmapViewOfFile(addr)) {
            fprintf(stderr, "warning: UnmapViewOfFile failed: %s\n",
                    llama_v2_format_win_err(GetLastError()).c_str());
        }
    }
#else
    static constexpr bool SUPPORTED = false;

    llama_v2_mmap(struct llama_v2_file *, bool prefetch = true) {
        (void)prefetch;
        throw std::runtime_error(std::string("mmap not supported"));
    }
#endif
};

// Represents some region of memory being locked using mlock or VirtualLock;
// will automatically unlock on destruction.
struct llama_v2_mlock {
    void * addr = NULL;
    size_t size = 0;
    bool failed_already = false;

    llama_v2_mlock() {}
    llama_v2_mlock(const llama_v2_mlock &) = delete;

    ~llama_v2_mlock() {
        if (size) {
            raw_unlock(addr, size);
        }
    }

    void init(void * addr) {
        LLAMA_V2_ASSERT(this->addr == NULL && this->size == 0);
        this->addr = addr;
    }

    void grow_to(size_t target_size) {
        LLAMA_V2_ASSERT(addr);
        if (failed_already) {
            return;
        }
        size_t granularity = lock_granularity();
        target_size = (target_size + granularity - 1) & ~(granularity - 1);
        if (target_size > size) {
            if (raw_lock((uint8_t *) addr + size, target_size - size)) {
                size = target_size;
            } else {
                failed_already = true;
            }
        }
    }

#ifdef _POSIX_MEMLOCK_RANGE
    static constexpr bool SUPPORTED = true;

    size_t lock_granularity() {
        return (size_t) sysconf(_SC_PAGESIZE);
    }

    #ifdef __APPLE__
        #define MLOCK_SUGGESTION \
            "Try increasing the sysctl values 'vm.user_wire_limit' and 'vm.global_user_wire_limit' and/or " \
            "decreasing 'vm.global_no_user_wire_amount'.  Also try increasing RLIMIT_MLOCK (ulimit -l).\n"
    #else
        #define MLOCK_SUGGESTION \
            "Try increasing RLIMIT_MLOCK ('ulimit -l' as root).\n"
    #endif

    bool raw_lock(const void * addr, size_t size) {
        if (!mlock(addr, size)) {
            return true;
        } else {
            char* errmsg = std::strerror(errno);
            bool suggest = (errno == ENOMEM);

            // Check if the resource limit is fine after all
            struct rlimit lock_limit;
            if (suggest && getrlimit(RLIMIT_MEMLOCK, &lock_limit))
                suggest = false;
            if (suggest && (lock_limit.rlim_max > lock_limit.rlim_cur + size))
                suggest = false;

            fprintf(stderr, "warning: failed to mlock %zu-byte buffer (after previously locking %zu bytes): %s\n%s",
                    size, this->size, errmsg, suggest ? MLOCK_SUGGESTION : "");
            return false;
        }
    }

    #undef MLOCK_SUGGESTION

    void raw_unlock(void * addr, size_t size) {
        if (munlock(addr, size)) {
            fprintf(stderr, "warning: failed to munlock buffer: %s\n", std::strerror(errno));
        }
    }
#elif defined(_WIN32)
    static constexpr bool SUPPORTED = true;

    size_t lock_granularity() {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        return (size_t) si.dwPageSize;
    }

    bool raw_lock(void * addr, size_t size) {
        for (int tries = 1; ; tries++) {
            if (VirtualLock(addr, size)) {
                return true;
            }
            if (tries == 2) {
                fprintf(stderr, "warning: failed to VirtualLock %zu-byte buffer (after previously locking %zu bytes): %s\n",
                        size, this->size, llama_v2_format_win_err(GetLastError()).c_str());
                return false;
            }

            // It failed but this was only the first try; increase the working
            // set size and try again.
            SIZE_T min_ws_size, max_ws_size;
            if (!GetProcessWorkingSetSize(GetCurrentProcess(), &min_ws_size, &max_ws_size)) {
                fprintf(stderr, "warning: GetProcessWorkingSetSize failed: %s\n",
                        llama_v2_format_win_err(GetLastError()).c_str());
                return false;
            }
            // Per MSDN: "The maximum number of pages that a process can lock
            // is equal to the number of pages in its minimum working set minus
            // a small overhead."
            // Hopefully a megabyte is enough overhead:
            size_t increment = size + 1048576;
            // The minimum must be <= the maximum, so we need to increase both:
            min_ws_size += increment;
            max_ws_size += increment;
            if (!SetProcessWorkingSetSize(GetCurrentProcess(), min_ws_size, max_ws_size)) {
                fprintf(stderr, "warning: SetProcessWorkingSetSize failed: %s\n",
                        llama_v2_format_win_err(GetLastError()).c_str());
                return false;
            }
        }
    }

    void raw_unlock(void * addr, size_t size) {
        if (!VirtualUnlock(addr, size)) {
            fprintf(stderr, "warning: failed to VirtualUnlock buffer: %s\n",
                    llama_v2_format_win_err(GetLastError()).c_str());
        }
    }
#else
    static constexpr bool SUPPORTED = false;

    size_t lock_granularity() {
        return (size_t) 65536;
    }

    bool raw_lock(const void * addr, size_t size) {
        fprintf(stderr, "warning: mlock not supported on this system\n");
        return false;
    }

    void raw_unlock(const void * addr, size_t size) {}
#endif
};

// Replacement for std::vector<uint8_t> that doesn't require zero-initialization.
struct llama_v2_buffer {
    uint8_t * addr = NULL;
    size_t size = 0;

    llama_v2_buffer() = default;

    void resize(size_t size) {
        delete[] addr;
        addr = new uint8_t[size];
        this->size = size;
    }

    ~llama_v2_buffer() {
        delete[] addr;
    }

    // disable copy and move
    llama_v2_buffer(const llama_v2_buffer&) = delete;
    llama_v2_buffer(llama_v2_buffer&&) = delete;
    llama_v2_buffer& operator=(const llama_v2_buffer&) = delete;
    llama_v2_buffer& operator=(llama_v2_buffer&&) = delete;
};

#ifdef GGML_USE_CUBLAS
#include "ggml_v2-cuda.h"
struct llama_v2_ctx_buffer {
    uint8_t * addr = NULL;
    bool is_cuda;
    size_t size = 0;

    llama_v2_ctx_buffer() = default;

    void resize(size_t size) {
        free();

        addr = (uint8_t *) ggml_v2_cuda_host_malloc(size);
        if (addr) {
            is_cuda = true;
        }
        else {
            // fall back to pageable memory
            addr = new uint8_t[size];
            is_cuda = false;
        }
        this->size = size;
    }

    void free() {
        if (addr) {
            if (is_cuda) {
                ggml_v2_cuda_host_free(addr);
            }
            else {
                delete[] addr;
            }
        }
        addr = NULL;
    }

    ~llama_v2_ctx_buffer() {
        free();
    }

    // disable copy and move
    llama_v2_ctx_buffer(const llama_v2_ctx_buffer&) = delete;
    llama_v2_ctx_buffer(llama_v2_ctx_buffer&&) = delete;
    llama_v2_ctx_buffer& operator=(const llama_v2_ctx_buffer&) = delete;
    llama_v2_ctx_buffer& operator=(llama_v2_ctx_buffer&&) = delete;
};
#else
typedef llama_v2_buffer llama_v2_ctx_buffer;
#endif

#endif