#include "time_offsets.h"
R__LOAD_LIBRARY(libtime_offsets.so)

void time_offsets(const char * filename)
{
  if ( filename != NULL)
    {
      pfileopen(filename);
    }
}
