#include "common/crypto/random.h"

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <bcrypt.h>
#  pragma comment(lib, "bcrypt.lib")
#elif defined(__APPLE__)
#  include <sys/random.h>
#else
#  include <sys/random.h>
#  include <errno.h>
#  include <fcntl.h>
#  include <unistd.h>
#endif

namespace deskbeam::crypto {

bool random_bytes(uint8_t* out, size_t len) {
    if (len == 0) return true;
    if (!out) return false;

#if defined(_WIN32)
    // BCRYPT_USE_SYSTEM_PREFERRED_RNG uses the kernel CSPRNG (since Win7).
    // STATUS_SUCCESS is 0; any nonzero return is failure.
    NTSTATUS st = BCryptGenRandom(nullptr, out, static_cast<ULONG>(len),
                                  BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return st == 0;
#elif defined(__APPLE__)
    // getentropy is capped at 256 bytes per call on Darwin.
    size_t off = 0;
    while (off < len) {
        size_t chunk = len - off;
        if (chunk > 256) chunk = 256;
        if (::getentropy(out + off, chunk) != 0) return false;
        off += chunk;
    }
    return true;
#else
    // Linux: prefer getrandom(); fall back to /dev/urandom on old kernels
    // or inside seccomp-restricted sandboxes that filter the syscall.
    size_t off = 0;
    while (off < len) {
        ssize_t n = ::getrandom(out + off, len - off, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            // Fallback path: open /dev/urandom and read the rest.
            int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
            if (fd < 0) return false;
            while (off < len) {
                ssize_t r = ::read(fd, out + off, len - off);
                if (r < 0) {
                    if (errno == EINTR) continue;
                    ::close(fd);
                    return false;
                }
                off += static_cast<size_t>(r);
            }
            ::close(fd);
            return true;
        }
        off += static_cast<size_t>(n);
    }
    return true;
#endif
}

} // namespace deskbeam::crypto
