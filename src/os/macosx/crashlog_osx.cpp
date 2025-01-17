/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file crashlog_osx.cpp OS X crash log handler */

#include "../../stdafx.h"
#include "../../crashlog.h"
#include "../../string_func.h"
#include "../../gamelog.h"
#include "../../saveload/saveload.h"
#include "../../video/video_driver.hpp"
#include "macos.h"

#include <errno.h>
#include <signal.h>
#include <mach-o/arch.h>
#include <dlfcn.h>
#include <cxxabi.h>

#include "../../safeguards.h"


/* Macro testing a stack address for valid alignment. */
#if defined(__i386__)
#define IS_ALIGNED(addr) (((uintptr_t)(addr) & 0xf) == 8)
#else
#define IS_ALIGNED(addr) (((uintptr_t)(addr) & 0xf) == 0)
#endif

/* printf format specification for 32/64-bit addresses. */
#ifdef __LP64__
#define PRINTF_PTR "0x%016lx"
#else
#define PRINTF_PTR "0x%08lx"
#endif

#define MAX_STACK_FRAMES 64

/**
 * OSX implementation for the crash logger.
 */
class CrashLogOSX : public CrashLog {
	/** Signal that has been thrown. */
	int signum;

	char filename_log[MAX_PATH];        ///< Path of crash.log
	char filename_save[MAX_PATH];       ///< Path of crash.sav
	char filename_screenshot[MAX_PATH]; ///< Path of crash.(png|bmp|pcx)

	char *LogOSVersion(char *buffer, const char *last) const override
	{
		int ver_maj, ver_min, ver_bug;
		GetMacOSVersion(&ver_maj, &ver_min, &ver_bug);

		const NXArchInfo *arch = NXGetLocalArchInfo();

		return buffer + seprintf(buffer, last,
				"Operating system:\n"
				" Name:     Mac OS X\n"
				" Release:  %d.%d.%d\n"
				" Machine:  %s\n"
				" Min Ver:  %d\n"
				" Max Ver:  %d\n",
				ver_maj, ver_min, ver_bug,
				arch != nullptr ? arch->description : "unknown",
				MAC_OS_X_VERSION_MIN_REQUIRED,
				MAC_OS_X_VERSION_MAX_ALLOWED
		);
	}

	char *LogError(char *buffer, const char *last, const char *message) const override
	{
		return buffer + seprintf(buffer, last,
				"Crash reason:\n"
				" Signal:  %s (%d)\n"
				" Message: %s\n\n",
				strsignal(this->signum),
				this->signum,
				message
		);
	}

	char *LogStacktrace(char *buffer, const char *last) const override
	{
		/* As backtrace() is only implemented in 10.5 or later,
		 * we're rolling our own here. Mostly based on
		 * http://stackoverflow.com/questions/289820/getting-the-current-stack-trace-on-mac-os-x
		 * and some details looked up in the Darwin sources. */
		buffer += seprintf(buffer, last, "\nStacktrace:\n");

		void **frame;
#if defined(__ppc__) || defined(__ppc64__)
		/* Apple says __builtin_frame_address can be broken on PPC. */
		__asm__ volatile("mr %0, r1" : "=r" (frame));
#else
		frame = (void **)__builtin_frame_address(0);
#endif

		for (int i = 0; frame != nullptr && i < MAX_STACK_FRAMES; i++) {
			/* Get IP for current stack frame. */
#if defined(__ppc__) || defined(__ppc64__)
			void *ip = frame[2];
#else
			void *ip = frame[1];
#endif
			if (ip == nullptr) break;

			/* Print running index. */
			buffer += seprintf(buffer, last, " [%02d]", i);

			Dl_info dli;
			bool dl_valid = dladdr(ip, &dli) != 0;

			const char *fname = "???";
			if (dl_valid && dli.dli_fname) {
				/* Valid image name? Extract filename from the complete path. */
				const char *s = strrchr(dli.dli_fname, '/');
				if (s != nullptr) {
					fname = s + 1;
				} else {
					fname = dli.dli_fname;
				}
			}
			/* Print image name and IP. */
			buffer += seprintf(buffer, last, " %-20s " PRINTF_PTR, fname, (uintptr_t)ip);

			/* Print function offset if information is available. */
			if (dl_valid && dli.dli_sname != nullptr && dli.dli_saddr != nullptr) {
				/* Try to demangle a possible C++ symbol. */
				int status = -1;
				char *func_name = abi::__cxa_demangle(dli.dli_sname, nullptr, 0, &status);

				long int offset = (intptr_t)ip - (intptr_t)dli.dli_saddr;
				buffer += seprintf(buffer, last, " (%s + %ld)", func_name != nullptr ? func_name : dli.dli_sname, offset);

				free(func_name);
			}
			buffer += seprintf(buffer, last, "\n");

			/* Get address of next stack frame. */
			void **next = (void **)frame[0];
			/* Frame address not increasing or not aligned? Broken stack, exit! */
			if (next <= frame || !IS_ALIGNED(next)) break;
			frame = next;
		}

		return buffer + seprintf(buffer, last, "\n");
	}

public:
	/**
	 * A crash log is always generated by signal.
	 * @param signum the signal that was caused by the crash.
	 */
	CrashLogOSX(int signum) : signum(signum)
	{
		filename_log[0] = '\0';
		filename_save[0] = '\0';
		filename_screenshot[0] = '\0';
	}

	/** Generate the crash log. */
	bool MakeCrashLog()
	{
		char buffer[65536];
		bool ret = true;

		printf("Crash encountered, generating crash log...\n");
		this->FillCrashLog(buffer, lastof(buffer));
		printf("%s\n", buffer);
		printf("Crash log generated.\n\n");

		printf("Writing crash log to disk...\n");
		if (!this->WriteCrashLog(buffer, filename_log, lastof(filename_log))) {
			filename_log[0] = '\0';
			ret = false;
		}

		printf("Writing crash savegame...\n");
		if (!this->WriteSavegame(filename_save, lastof(filename_save))) {
			filename_save[0] = '\0';
			ret = false;
		}

		printf("Writing crash screenshot...\n");
		if (!this->WriteScreenshot(filename_screenshot, lastof(filename_screenshot))) {
			filename_screenshot[0] = '\0';
			ret = false;
		}

		return ret;
	}

	/** Show a dialog with the crash information. */
	void DisplayCrashDialog() const
	{
		static const char crash_title[] =
			"A serious fault condition occurred in the game. The game will shut down.";

		char message[1024];
		seprintf(message, lastof(message),
				 "Please send the generated crash information and the last (auto)save to the developers. "
				 "This will greatly help debugging. The correct place to do this is https://github.com/OpenTTD/OpenTTD/issues.\n\n"
				 "Generated file(s):\n%s\n%s\n%s",
				 this->filename_log, this->filename_save, this->filename_screenshot);

		ShowMacDialog(crash_title, message, "Quit");
	}
};

/** The signals we want our crash handler to handle. */
static const int _signals_to_handle[] = { SIGSEGV, SIGABRT, SIGFPE, SIGBUS, SIGILL, SIGSYS };

/**
 * Entry point for the crash handler.
 * @note Not static so it shows up in the backtrace.
 * @param signum the signal that caused us to crash.
 */
void CDECL HandleCrash(int signum)
{
	/* Disable all handling of signals by us, so we don't go into infinite loops. */
	for (const int *i = _signals_to_handle; i != endof(_signals_to_handle); i++) {
		signal(*i, SIG_DFL);
	}

	if (GamelogTestEmergency()) {
		ShowMacDialog("A serious fault condition occurred in the game. The game will shut down.",
				"As you loaded an emergency savegame no crash information will be generated.\n",
				"Quit");
		abort();
	}

	if (SaveloadCrashWithMissingNewGRFs()) {
		ShowMacDialog("A serious fault condition occurred in the game. The game will shut down.",
				"As you loaded an savegame for which you do not have the required NewGRFs no crash information will be generated.\n",
				"Quit");
		abort();
	}

	CrashLogOSX log(signum);
	log.MakeCrashLog();
	if (VideoDriver::GetInstance() == nullptr || VideoDriver::GetInstance()->HasGUI()) {
		log.DisplayCrashDialog();
	}

	CrashLog::AfterCrashLogCleanup();
	abort();
}

/* static */ void CrashLog::InitialiseCrashLog()
{
	for (const int *i = _signals_to_handle; i != endof(_signals_to_handle); i++) {
		signal(*i, HandleCrash);
	}
}

/* static */ void CrashLog::InitThread()
{
}
