#include "gain_hv.h"
R__LOAD_LIBRARY(libgain_hv.so)

void gain_hv(const char * filename)
{
  if ( filename != NULL)
    {
      pfileopen(filename);
    }
}
