#include "general.h"
R__LOAD_LIBRARY(libgeneral.so)

void general(const char * filename)
{
  if ( filename != NULL)
    {
      pfileopen(filename);
    }
}
