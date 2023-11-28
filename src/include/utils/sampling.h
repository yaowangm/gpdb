/*-------------------------------------------------------------------------
 *
 * sampling.h
 *	  definitions for sampling functions
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/sampling.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SAMPLING_H
#define SAMPLING_H

#include "storage/block.h"		/* for typedef BlockNumber */


/* Random generator for sampling code */
typedef unsigned short SamplerRandomState[3];

extern void sampler_random_init_state(long seed,
									  SamplerRandomState randstate);
extern double sampler_random_fract(SamplerRandomState randstate);

/* Block sampling methods */

/* Data structure for Algorithm S from Knuth 3.4.2 */
typedef struct
{
	BlockNumber N;				/* number of blocks, known in advance */
	int			n;				/* desired sample size */
	BlockNumber t;				/* current block number */
	int			m;				/* blocks selected so far */
	SamplerRandomState randstate;	/* random generator state */
} BlockSamplerData;

typedef BlockSamplerData *BlockSampler;

extern BlockNumber BlockSampler_Init(BlockSampler bs, BlockNumber nblocks,
									 int samplesize, long randseed);
extern bool BlockSampler_HasMore(BlockSampler bs);
extern BlockNumber BlockSampler_Next(BlockSampler bs);

/* 64 bit version of BlockSampler (used for sampling AO/CO table rows) */
typedef struct
{
	int64           N;				/* number of objects, known in advance */
	int64			n;				/* desired sample size */
	int64           t;				/* current object number */
	int64			m;				/* objects selected so far */
	SamplerRandomState randstate;	/* random generator state */
} RowSamplerData;

typedef RowSamplerData *RowSampler;

extern void RowSampler_Init(RowSampler rs, int64 nobjects,
							   int64 samplesize, long randseed);
extern bool RowSampler_HasMore(RowSampler rs);
extern int64 RowSampler_Next(RowSampler rs);

/* Reservoir sampling methods */

typedef struct
{
	double		W;
	SamplerRandomState randstate;	/* random generator state */
} ReservoirStateData;

typedef ReservoirStateData *ReservoirState;

extern void reservoir_init_selection_state(ReservoirState rs, int n);
extern double reservoir_get_next_S(ReservoirState rs, double t, int n);

/* Old API, still in use by assorted FDWs */
/* For backwards compatibility, these declarations are duplicated in vacuum.h */

extern double anl_random_fract(void);
extern double anl_init_selection_state(int n);
extern double anl_get_next_S(double t, int n, double *stateptr);

/*
 * Variable Step Length Sampling algorithm
 *
 * For a skewed table with n tuples, select the middle tuple (precisely,
 * the 2^(log2(n)) tuple), which will divide the table to two parties; select
 * the middle of the first part and second part, and repeat the process, till
 * enough live tuples are selected, or the entire table is scanned.
 * In addition, a random start offset is used to avoid same result for every
 * sampling.
 *
 * The approach can ensure required number of rows will be selected without
 * unnecessary checking visibility for others.
 *
 * e.g.
 *   1. Give n = 21793: we have rows no. 0,1,2,...21292
 *   2. set step length l = pow (2, (int)log2(21793)) = pow (2, 14) = 16384
 *   3. Scan the array with step of l: read row 16384.
 *   4. Scan the array with step of l/2 = 8192, but ignore any page which number
 *     can be divided exactly be l = 16384: read row 8192
 *   5. Scan the array with step of l/4 = 4096, but ignore any row which number
 *     can be divided exactly be l/2 = 8192: read row 4096 (4096 * 1), 12288
 *     (4096 * 3), 20480 (4096 * 4)
 *   6. Scan the array with step of l/8 = 2048, but ignore any row which number
 *      can be divided exactly be l/4 = 4096: read row 2048 (2048 * 1), 6144
 *     (2048 * 3), 10240 (2048 * 5), 14336 (2048 * 7), 18432 (2048 * 9)
 *   7. ......
 *   8. (If the table is almost empty) we get step length of 1, and scan thexi
 *     array with step 1, but ignore all even rows.
 */
typedef struct
{
	int64           N;				/* number of objects, known in advance */
	int64			n;				/* desired sample size */
	int64           t;				/* current object number */
	int64			m;				/* objects selected so far */
	int64			pos;			/* current position */
	int64			stepLength;
	int64			startOffset;
	SamplerRandomState randstate;	/* random generator state */
} VslSamplerData;

typedef VslSamplerData *VslSampler;

extern void VslSampler_Init(VslSampler vs,
							int64 nobjects,
							int64 samplesize,
							int64 randseed);
extern bool VslSampler_HasMore(VslSampler vs);
extern int64 VslSampler_Next(VslSampler vs);
extern void VslSampler_SetValid(VslSampler vs);

#endif							/* SAMPLING_H */
