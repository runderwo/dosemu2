#include <stdio.h>


__asm__(".org 0x110000");


#ifdef __linux__
int
main(int argc, char **argv)
{
/* just run this function */
  int emulate(int, char **);

  emulate(argc, argv);
}
#endif
