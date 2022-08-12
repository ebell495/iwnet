#pragma once
#ifndef _IWNET_H
#define _IWNET_H

#include <iowow/basedefs.h>

IW_EXTERN_C_START

#define IWN_IPV4 0x01U
#define IWN_IPV6 0x02U
#define IWN_TCP  0x04U
#define IWN_UDP  0x08U

extern const char *iwn_cacerts;
extern const size_t iwn_cacerts_len;

IW_EXTERN_C_END
#endif
