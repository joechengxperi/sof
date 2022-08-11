/* SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright(c) 2022 Intel Corporation. All rights reserved.
 */

#define LENGTH_BLACKMAN 160

static const int16_t ref_blackman[LENGTH_BLACKMAN] = {
	0,
	13,
	51,
	115,
	204,
	319,
	458,
	623,
	812,
	1025,
	1263,
	1524,
	1808,
	2115,
	2444,
	2795,
	3167,
	3560,
	3973,
	4405,
	4856,
	5325,
	5811,
	6314,
	6832,
	7366,
	7913,
	8474,
	9047,
	9631,
	10226,
	10831,
	11444,
	12065,
	12693,
	13326,
	13965,
	14607,
	15251,
	15898,
	16545,
	17192,
	17838,
	18482,
	19122,
	19758,
	20389,
	21013,
	21631,
	22240,
	22840,
	23429,
	24008,
	24575,
	25129,
	25670,
	26196,
	26706,
	27201,
	27678,
	28138,
	28580,
	29003,
	29406,
	29788,
	30150,
	30490,
	30808,
	31104,
	31376,
	31625,
	31851,
	32052,
	32229,
	32381,
	32508,
	32610,
	32686,
	32737,
	32763,
	32763,
	32737,
	32686,
	32609,
	32507,
	32380,
	32228,
	32051,
	31850,
	31624,
	31375,
	31102,
	30807,
	30489,
	30148,
	29787,
	29404,
	29001,
	28578,
	28137,
	27676,
	27199,
	26704,
	26193,
	25667,
	25127,
	24572,
	24005,
	23426,
	22837,
	22237,
	21627,
	21010,
	20385,
	19755,
	19118,
	18478,
	17834,
	17189,
	16541,
	15894,
	15247,
	14602,
	13960,
	13322,
	12688,
	12060,
	11439,
	10826,
	10221,
	9626,
	9041,
	8468,
	7908,
	7360,
	6827,
	6308,
	5805,
	5319,
	4850,
	4399,
	3967,
	3554,
	3161,
	2788,
	2437,
	2108,
	1801,
	1517,
	1256,
	1018,
	805,
	615,
	451,
	311,
	197,
	107,
	43,
	5,
	-8,
};
