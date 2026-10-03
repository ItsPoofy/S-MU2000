// license:BSD-3-Clause
//
// S-MU2000: the executable-memory layer the JITs sit on.
//
//   alloc_rw(size)       anonymous RW memory (staging and code buffers)
//   alloc_rwx(size)      RWX memory for code that is written and run in place
//   make_writable(b, s)  re-protect a buffer for writing (rebuilds)
//   make_executable(b, s) re-protect it for execution
//   free_mem(b, s)
//   copy_code(dst, src, n) copy into executable memory (brackets write protect)
//   flush_code(b, s)     make written code visible to the instruction fetch
//
// RWX is fine for the tools here: they are not hardened-runtime processes, so
// macOS maps MAP_JIT pages RWX and Linux allows the same. Code that wants to
// survive hardening uses the alloc_rw + make_writable/make_executable dance
//
// Copy-in always goes through make_writable → memcpy → make_executable, so a
// rebuild of an already-executable buffer works on every platform. Nothing is
// ever persisted: the generated code lives in anonymous memory and dies with
// the process (upstream's rule: no firmware-derived data on disk).
//
// macOS maps with MAP_JIT (arm64 refuses a plain RWX mapping otherwise). A
// MAP_JIT region is execute-only to the writing thread until its write
// protection is lifted, so every copy into it is bracketed by write_begin() /
// write_end() -- see copy_code(). The toggle is per thread: the SH2 JIT and the
// MEG JIT both compile on whichever thread calls them, and each write is
// bracketed, so this is safe. Linux uses plain anonymous mmap.

#ifndef S_MU2000_EXEC_MEM_H
#define S_MU2000_EXEC_MEM_H

#include "mamecompat.h"

#if defined(_WIN32)

#include <windows.h>

namespace exec_mem {

inline void *alloc_rw(size_t size)
{
	return VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}

inline void *alloc_rwx(size_t size)
{
	return VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
}

inline bool make_writable(void *buf, size_t size)
{
	DWORD old;
	return VirtualProtect(buf, size, PAGE_READWRITE, &old) != 0;
}

inline bool make_executable(void *buf, size_t size)
{
	DWORD old;
	return VirtualProtect(buf, size, PAGE_EXECUTE_READ, &old) != 0;
}

inline void free_mem(void *buf, size_t)
{
	VirtualFree(buf, 0, MEM_RELEASE);
}

// x86 caches are coherent with stores; nothing to do
inline void flush_code(void *, size_t) {}

inline void write_begin() {}
inline void write_end() {}

} // namespace exec_mem

#else

#include <sys/mman.h>

#ifdef __APPLE__
#include <pthread.h>
#include <TargetConditionals.h>
// pthread_jit_write_protect_np exists on macOS and is marked unavailable on iOS,
// so the write-protection dance is macOS-only. MAP_JIT itself is still defined on
// iOS (it is what a JIT would map), it just has no per-thread toggle to drive.
#if !TARGET_OS_IPHONE
#define SMU2000_JIT_WRITE_PROTECT 1
#endif
#ifndef MAP_JIT
#define MAP_JIT 0x800
#endif
#define SMU2000_MMAP_EXTRA MAP_JIT
#else
#define SMU2000_MMAP_EXTRA 0
#endif

namespace exec_mem {

// On arm64, instruction caches are not coherent: after copying generated code
// the dcache lines holding it must be cleaned and the icache invalidated
// before execution. make_executable() runs this for the caller.
#ifdef __aarch64__
#ifdef __APPLE__
#include <libkern/OSCacheControl.h>
inline void flush_code(void *buf, size_t size) { sys_icache_invalidate(buf, size); }
#else
inline void flush_code(void *buf, size_t size) { __builtin___clear_cache((char *)buf, (char *)buf + size); }
#endif
#else
inline void flush_code(void *, size_t) {}
#endif

// Lift the write protection of MAP_JIT pages for the calling thread, and put
// it back so the same thread can run the code again. Nothing on other
// platforms (there it is a plain mmap and the stores are ordinary).
//
// **iOS has no per-thread JIT write protection.** pthread_jit_write_protect_np
// is marked unavailable in the iOS SDK, so it cannot be called there at all - the
// first iOS build failed on exactly this. On macOS arm64 a MAP_JIT region is
// execute-only to the writing thread until the toggle lifts it; on iOS there is
// no toggle, so the pages stay writable and the stores are ordinary.
//
// That is only a JIT feature. The interpreter path does not use exec_mem at all
// and is fully working, so iOS builds and runs without the JIT; see
// doc/ios-auv3.md.
#if defined(__aarch64__) && defined(__APPLE__) && defined(SMU2000_JIT_WRITE_PROTECT)
inline void write_begin() { pthread_jit_write_protect_np(0); }
inline void write_end()   { pthread_jit_write_protect_np(1); }
#else
inline void write_begin() {}
inline void write_end() {}
#endif

inline void *alloc_rw(size_t size)
{
	void *p = mmap(nullptr, size, PROT_READ | PROT_WRITE,
	               MAP_PRIVATE | MAP_ANONYMOUS | SMU2000_MMAP_EXTRA, -1, 0);
	return p == MAP_FAILED ? nullptr : p;
}

inline void *alloc_rwx(size_t size)
{
	void *p = mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC,
	               MAP_PRIVATE | MAP_ANONYMOUS | SMU2000_MMAP_EXTRA, -1, 0);
	return p == MAP_FAILED ? nullptr : p;
}

inline bool make_writable(void *buf, size_t size)
{
	write_begin();
	return mprotect(buf, size, PROT_READ | PROT_WRITE) == 0;
}

inline bool make_executable(void *buf, size_t size)
{
	if (mprotect(buf, size, PROT_READ | PROT_EXEC) != 0)
		return false;
	flush_code(buf, size);
	write_end();
	return true;
}

// Copy code into an executable buffer: writable for the duration of the copy,
// executable (and cache-coherent) again afterwards.
inline void copy_code(void *dst, const void *src, size_t size)
{
	write_begin();
	std::memcpy(dst, src, size);
	flush_code(dst, size);
	write_end();
}

inline void free_mem(void *buf, size_t size)
{
	munmap(buf, size);
}

} // namespace exec_mem

#undef SMU2000_MMAP_EXTRA

#endif

#endif // S_MU2000_EXEC_MEM_H
