/* (PS5) What the console's C library does not have and host/ uses, so host/'s files stay as upstream writes them.
   ps5/build.py includes this ahead of every C unit of host/. */
#ifndef HOS_PS5_C_COMPAT_H
#define HOS_PS5_C_COMPAT_H
#include <sys/types.h>   /* (ssize_t, which glibc's headers bring in by themselves) */
#endif
