/*
 * Stand-in for Haiku's libroot.so, so the image's libfreetype.so loads on
 * Linux (check.sh). It defines nothing FreeType uses: every symbol the library
 * imports is plain C library -- malloc, memcpy, fopen, setjmp and the like --
 * and resolves from glibc, which a NEEDED entry on libc pulls in. glibc's
 * aarch64 jmp_buf (312 bytes) fits in Haiku's (520, headers/posix/setjmp.h),
 * so setjmp writes no further than FreeType's buffer reaches.
 */
void
haiku_remote_libroot_stub(void)
{
}
