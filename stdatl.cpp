#include "stdatl.h"

#if (_ATL_VER < 0x0700) && !defined(_WIN32_WCE)
#include <atlimpl.cpp>
#endif //(_ATL_VER < 0x0700)

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ws2_32.lib")
