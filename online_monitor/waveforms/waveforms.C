#include "waveforms.h"
R__LOAD_LIBRARY(libwaveforms.so)

void waveforms(const char * filename)
{
  if ( filename != NULL)
    {
      pfileopen(filename);
    }
}
