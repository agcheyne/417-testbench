#include "timing_xtalk.h"
R__LOAD_LIBRARY(libtiming_xtalk.so)

void timing_xtalk(const char * filename)
{
  if ( filename != NULL)
    {
      pfileopen(filename);
    }
}
