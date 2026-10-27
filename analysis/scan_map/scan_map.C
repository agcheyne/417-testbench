#include "scan_map.h"
R__LOAD_LIBRARY(libscan_map.so)

void scan_map(const char * filename)
{
  if ( filename != NULL)
    {
      pfileopen(filename);
    }
}
