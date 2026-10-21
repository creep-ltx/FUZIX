#include <stdio.h>
#include <unistd.h>
#include <utime.h>

/*
 *	Wrap the kernel utime call: the kernel takes each time as two 32bit
 *	halves, low then high (__ktime_t), not as a time_t. Passing the
 *	time_t pair straight through only worked on little-endian machines;
 *	on big-endian ones (68000, 6809...) the kernel read the high half
 *	as the time and every file went to 1970.
 */
int utime(const char *filename, const struct utimbuf *utim)
{
  __ktime_t t[2];
  if (utim == NULL)
    return _utime(filename, NULL);
  t[0].low = utim->actime;
  t[1].low = utim->modtime;
#ifndef NO_64BIT
  t[0].high = utim->actime >> 32;
  t[1].high = utim->modtime >> 32;
#else
  t[0].high = 0;
  t[1].high = 0;
#endif
  return _utime(filename, t);
}
