// sisadriver.ldd stand-in (see tablestub.h): the 5730's IscApi loads it (RAPIDO's ISA transport, where
// the N95's IscApi loads xbus.ldd); the fake modem link itself is nokiaiscdriver. Load only.
#include "tablestub.h"
NO_CONTROLS
TABLE_STUB_LDD("SisaDriver", KCtl, 0)
