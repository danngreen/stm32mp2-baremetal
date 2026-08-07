#include <cerrno>
#include <cstddef>
#include <sys/stat.h>

// =============================================================================
//  newlib_syscalls.cc -- the syscalls the real newlib/libstdc++ need
// =============================================================================
//
// This project links the actual C/C++ libraries (no -nostdlib), so malloc and
// operator new work for sketches. That costs a handful of hooks:
//
//  - _sbrk grows the heap: the .heap section in linkscript.ld
//    (_sheap.._eheap, sized __HEAP_SIZE), placed after .bss and before the
//    stacks, so exhaustion fails here with ENOMEM instead of eating the stack.
//  - the stdio ones exist because libstdc++'s error paths (e.g. a vector
//    growth failure) print to stderr before aborting. _write goes to the UART
//    so those messages are actually seen; there is nothing to read or seek.
//
// (_exit, _kill, _getpid, __errno are in shared/newlib/syscall_stubs.c.)

extern "C" {

extern char _sheap[], _eheap[];
void putchar_s(char c); // shared/print/uart_print.c

void *_sbrk(ptrdiff_t incr)
{
	static char *brk = _sheap;
	if (brk + incr > _eheap || brk + incr < _sheap) {
		errno = ENOMEM;
		return reinterpret_cast<void *>(-1);
	}
	char *const prev = brk;
	brk += incr;
	return prev;
}

int _write(int, const char *buf, int len)
{
	for (int i = 0; i < len; i++) {
		if (buf[i] == '\n')
			putchar_s('\r');
		putchar_s(buf[i]);
	}
	return len;
}

int _read(int, char *, int)
{
	return 0; // EOF
}

int _close(int)
{
	return -1;
}

int _lseek(int, int, int)
{
	return 0;
}

int _fstat(int, struct stat *st)
{
	st->st_mode = S_IFCHR; // character device -> stdio stays line-buffered
	return 0;
}

int _isatty(int)
{
	return 1;
}
}
