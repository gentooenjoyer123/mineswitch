#ifndef MINESWITCH_TEST_NEWLIB_H
#define MINESWITCH_TEST_NEWLIB_H
/* Host allocator tests use only the ordinary, separate-object Newlib malloc.
 * The installed target ABI is independently checked by ../build.py. */
#define _NEWLIB_VERSION "4.6.0"
#endif
