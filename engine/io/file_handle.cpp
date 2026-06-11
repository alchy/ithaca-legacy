// engine/io/file_handle.cpp — viz file_handle.h.
#include "io/file_handle.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ithaca {
namespace {

#if defined(_WIN32)
// Win32: ReadFile se sync handle + OVERLAPPED offsetem = pozicovane cteni
// bez posunu file pointeru (ekvivalent pread).
class Win32FileHandle : public IFileHandle {
public:
    Win32FileHandle(HANDLE h, uint64_t size) : h_(h), size_(size) {}
    ~Win32FileHandle() override { CloseHandle(h_); }
    bool readAt(uint64_t off, void* buf, size_t n) const override {
        uint8_t* p = static_cast<uint8_t*>(buf);
        while (n > 0) {
            OVERLAPPED ov{};
            ov.Offset     = (DWORD)(off & 0xFFFFFFFFu);
            ov.OffsetHigh = (DWORD)(off >> 32);
            DWORD chunk = (n > 0x40000000u) ? 0x40000000u : (DWORD)n;
            DWORD got = 0;
            if (!ReadFile(h_, p, chunk, &got, &ov) || got == 0) return false;
            p += got; off += got; n -= got;
        }
        return true;
    }
    uint64_t size() const override { return size_; }
private:
    HANDLE   h_;
    uint64_t size_;
};
#else
class PosixFileHandle : public IFileHandle {
public:
    PosixFileHandle(int fd, uint64_t size) : fd_(fd), size_(size) {}
    ~PosixFileHandle() override { ::close(fd_); }
    bool readAt(uint64_t off, void* buf, size_t n) const override {
        uint8_t* p = static_cast<uint8_t*>(buf);
        while (n > 0) {
            ssize_t got = ::pread(fd_, p, n, (off_t)off);
            if (got <= 0) return false;   // 0 = EOF driv nez n bajtu
            p += got; off += (uint64_t)got; n -= (size_t)got;
        }
        return true;
    }
    uint64_t size() const override { return size_; }
private:
    int      fd_;
    uint64_t size_;
};
#endif

} // namespace

std::shared_ptr<IFileHandle> openFileHandle(const std::string& path) {
#if defined(_WIN32)
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return nullptr;
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz)) { CloseHandle(h); return nullptr; }
    return std::make_shared<Win32FileHandle>(h, (uint64_t)sz.QuadPart);
#else
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return nullptr;
    struct stat st{};
    if (::fstat(fd, &st) != 0) { ::close(fd); return nullptr; }
    return std::make_shared<PosixFileHandle>(fd, (uint64_t)st.st_size);
#endif
}

} // namespace ithaca
