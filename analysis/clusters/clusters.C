#include "clusters.h"
R__LOAD_LIBRARY(libclusters.so)

void clusters(const char * filename)
{
  if ( filename != NULL)
    {
      pfileopen(filename);
    }
}
