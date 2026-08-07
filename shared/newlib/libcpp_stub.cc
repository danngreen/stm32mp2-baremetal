namespace std
{
void __throw_bad_function_call()
{
	while (1)
		;
}
void __throw_bad_alloc()
{
	while (1)
		;
}
void __throw_length_error(char const *)
{
	while (1)
		;
}

// Needed when compiling at -O0 with gcc 15
__attribute__((noreturn)) void __glibcxx_assert_fail(char const *file, int, char const *line, char const *msg)
{
	// puts("assert failed: ");
	// puts(file);
	// puts(":");
	// puts(line);
	// puts(" ");
	// puts(msg);
	// puts("\n");
	while (1)
		;
}

} // namespace std

extern "C" void __cxa_pure_virtual()
{
	while (1)
		;
}

// A class with a virtual destructor gets a "deleting destructor" (D0) that
// calls operator delete, and the linker wants the symbol even when nothing
// ever deletes such an object. There is no heap here, so deleting is a bug:
// trap rather than pretend to free. Defined only if something references them.
void operator delete(void *) noexcept
{
	while (1)
		;
}
void operator delete(void *, unsigned long) noexcept
{
	while (1)
		;
}
void operator delete[](void *) noexcept
{
	while (1)
		;
}
void operator delete[](void *, unsigned long) noexcept
{
	while (1)
		;
}
