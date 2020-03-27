/* SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright(c) 2019 Intel Corporation. All rights reserved.
 *
 * Author: Karol Trzcinski <karolx.trzcinski@linux.intel.com>
 */

/**
 * \file include/sof/compiler_info.h
 * \brief Compiler version and name descriptor
 * \author Karol Trzcinski <karolx.trzcinski@linux.intel.com>
 */

#ifndef __ARCH_COMPILER_INFO_H__
#define __ARCH_COMPILER_INFO_H__

#include <xtensa/hal.h>

/* read used compilator name and version */
/* CC_NAME must consist of 3 characters with null termination */
/* See declaration of sof_ipc_cc_version. */
#define CC_MAJOR (XTHAL_RELEASE_MAJOR / 1000)
#define CC_MINOR ((XTHAL_RELEASE_MAJOR % 1000) / 10)
#define CC_MICRO XTHAL_RELEASE_MINOR
#define CC_NAME "XCC"
#define CC_DESC " XccFoo"

#endif /* __ARCH_COMPILER_INFO_H__ */
