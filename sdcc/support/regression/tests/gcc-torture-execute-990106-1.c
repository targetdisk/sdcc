/*
   990106-1.c from the execute part of the gcc torture suite.
 */

#include <testfwk.h>

#ifdef __SDCC
#pragma std_c99
#endif

int
foo(char *bufp)
{
    int x = 80;
    return (*bufp++ = x ? 'a' : 'b');
}

void
testTortureExecute (void)
{
  char x;

  ASSERT(foo (&x) == 'a');

  return;
}

