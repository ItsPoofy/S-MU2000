// license:BSD-3-Clause
//
// abort()・std::terminate で止まるときに、呼び出し元を書き残す（Windows の gui）。
//
// イベントログには「msvcrt.dll の abort」としか残らず、誰が abort を呼んだのか分からない。
// SIGABRT と std::terminate を受けて、そのときの呼び出しの並び（モジュール + オフセット）と、
// 例外で止まったならその what() を <設定>/S-MU2000/crash.txt に足していく。
// オフセットは nm -C --defined-only build/gui.exe の番地（0x140000000 起点）と突き合わせる
// （doc/debugging のメモ「落ちたらイベントログのフォールトオフセット＋nm」と同じやり方）。
#ifndef S_MU2000_COMPAT_CRASH_LOG_H
#define S_MU2000_COMPAT_CRASH_LOG_H
#pragma once

#if defined(_WIN32)

#include "compat/paths.h"

#include <windows.h>

#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <exception>
#include <string>
#include <typeinfo>

namespace smu2000::crash_log {

inline void write(const char *why)
{
	const std::string dir = smu2000::config_dir();
	if (dir.empty())
		return;
	std::FILE *f = std::fopen((dir + "crash.txt").c_str(), "ab");
	if (!f)
		return;
	const std::time_t t = std::time(nullptr);
	char when[64];
	std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
	std::fprintf(f, "%s  %s (thread %lu)\r\n", when, why, GetCurrentThreadId());
	void *frames[48];
	const USHORT n = CaptureStackBackTrace(0, 48, frames, nullptr);
	for (USHORT i = 0; i < n; i++) {
		HMODULE mod = nullptr;
		char name[MAX_PATH] = "?";
		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		                       static_cast<LPCSTR>(frames[i]), &mod) && mod) {
			char full[MAX_PATH];
			if (GetModuleFileNameA(mod, full, MAX_PATH)) {
				const char *base = std::strrchr(full, '\\');
				std::snprintf(name, sizeof(name), "%s", base ? base + 1 : full);
			}
		}
		const unsigned long long off = mod ? (unsigned long long)(reinterpret_cast<const char *>(frames[i]) -
		                                                         reinterpret_cast<const char *>(mod)) : 0;
		std::fprintf(f, "  #%-2u %s+0x%llx\r\n", unsigned(i), name, off);
	}
	std::fclose(f);
}

inline void on_abort(int)
{
	write("SIGABRT");
}

inline void on_terminate()
{
	std::string why = "std::terminate";
	if (const std::exception_ptr e = std::current_exception()) {
		try {
			std::rethrow_exception(e);
		} catch (const std::exception &x) {
			why += std::string(": ") + typeid(x).name() + ": " + x.what();
		} catch (...) {
			why += ": (not a std::exception)";
		}
	}
	write(why.c_str());
	std::abort();
}

inline void install()
{
	std::signal(SIGABRT, on_abort);
	std::set_terminate(on_terminate);
}

} // namespace smu2000::crash_log

#endif

#endif
