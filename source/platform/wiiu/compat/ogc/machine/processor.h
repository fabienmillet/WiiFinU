#pragma once
#include "../../gctypes.h"
#define _CPU_ISR_Disable(l) do { (l) = 0; } while (0)
#define _CPU_ISR_Restore(l) do { (void)(l); } while (0)
