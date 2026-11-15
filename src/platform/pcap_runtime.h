// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#pragma once
#ifdef __cplusplus
extern "C" {
#endif
/* Runtime binding keeps a missing capture driver from preventing GUI startup. */
int niceecatm_pcap_available(void);
const char* niceecatm_pcap_status(void);
const char* niceecatm_pcap_error(void);
const char* niceecatm_pcap_last_error(void);
#ifdef __cplusplus
}
#endif
