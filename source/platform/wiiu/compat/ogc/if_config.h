#pragma once
#include "../gctypes.h"
#ifdef __cplusplus
extern "C" {
#endif
s32 if_config(char* local_ip, char* netmask, char* gateway, bool use_dhcp, int max_retries);
#ifdef __cplusplus
}
#endif
