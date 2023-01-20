/*------------------------------------------------------------------------------
 *
 * bitmap_compression.c
 *	  Compression code tailored to compression of bitmap
 *
 * Copyright (c) 2013-Present VMware, Inc. or its affiliates.
 *
 *
 * IDENTIFICATION
 *	    src/backend/utils/misc/bitmap_compression.c
 *
 *------------------------------------------------------------------------------
*/
#include "postgres.h"
#include "utils/bitmap_compression.h"
#include "utils/bitstream.h"
#include "utils/guc.h"

typedef enum BitmapCompressionFlag
{
	BITMAP_COMPRESSION_FLAG_ZERO = 0x00,
	BITMAP_COMPRESSION_FLAG_ONE = 0x01,
	BITMAP_COMPRESSION_FLAG_RLE = 0x02,
	BITMAP_COMPRESSION_FLAG_RAW = 0x03
} BitmapCompressionFlag;

/* Controller to compress a particular bitmap block */
typedef struct BitmapCompressBlockController
{
	Bitstream	*bitstream;
	uint32		blockData;
	bool		isFirstBlock;
	uint32		lastBlockData;
	int			lastBlockFlag;
	int			rleRepeatCount;
} BitmapCompressBlockController;

static bool
Bitmap_CompressBlock(BitmapCompressBlockController *compBlockCtl);

static int
Bitmap_Compress_NoCompress(
		uint32* bitmap,
		int blockCount,
		Bitstream *bitstream);

static void
Bitmap_Compress_DefaultDecompress(BitmapDecompressState *state,
						   uint32 *bitmap);

static void
Bitmap_Compress_DecompressNone(BitmapDecompressState *state,
							   uint32 *bitmap);

/*
 * Initializes a new decompression run
 */ 
bool
BitmapDecompress_Init(
	BitmapDecompressState *state,
	unsigned char *inData, int inDataSize)
{
	uint32 tmp;

	Bitstream_Init(&state->bitstream, inData, inDataSize);

	if (!Bitstream_Get(&state->bitstream, 1, &tmp))
		return false;
	state->compressionType = tmp;

	if (!Bitstream_Skip(&state->bitstream, 3)) 
		return false;

	if (!Bitstream_Get(&state->bitstream, 12, &tmp))
		return false;
	state->blockCount = tmp;
	return true;
}

/*
 * returns the number of compression bitmap blocks
 */ 
int
BitmapDecompress_GetBlockCount(
	BitmapDecompressState *state)
{
	return state->blockCount;
}

/*
 * returns the used compression type
 */ 
BitmapCompressionType
BitmapDecompress_GetCompressionType(
	BitmapDecompressState *state)
{
	return state->compressionType;
}

/*
 * returns true iff the compression had an error
 */ 
int
BitmapDecompress_HasError(
	BitmapDecompressState *state)
{
	return Bitstream_HasError(&state->bitstream);
}

/*
 * Performs the bitmap decompression.
 *
 * bitmapDataSize in uint32-words.
 */
void
BitmapDecompress_Decompress(BitmapDecompressState *state,
							uint32 *bitmap,
							int bitmapDataSize)
{
	uint32 lastBlockData, flag, rleRepeatCount;
	int i;
	bool failed = false;

	Assert(state);
	Assert(Bitstream_GetOffset(&state->bitstream) == 16U);

	if (state->blockCount < 0 || state->blockCount > bitmapDataSize)
	{
		elog(ERROR, "invalid block count for bitmap during decompression: "
				"block count %d, compression type %d", 
				state->blockCount, state->compressionType);
	}

	if (state->compressionType == BITMAP_COMPRESSION_TYPE_NO)
	{
		Bitmap_Compress_DecompressNone(state, bitmap);
	}
	else if (state->compressionType == BITMAP_COMPRESSION_TYPE_DEFAULT)
	{
		Bitmap_Compress_DefaultDecompress(state, bitmap);
	}
	else
	{
		elog(ERROR, "illegal compression type during bitmap decompression: "
				"compression type %d", state->compressionType);
	}
}

static bool 
Bitmap_EncodeRLE(Bitstream* bitstream,
		int rleRepeatCount,
		int lastBlockFlag)
{
	int i;

	if (lastBlockFlag == BITMAP_COMPRESSION_FLAG_RAW || rleRepeatCount > 4)
	{
		if (!Bitstream_Put(bitstream, BITMAP_COMPRESSION_FLAG_RLE, 2))
			return false;
		if (!Bitstream_Put(bitstream, rleRepeatCount - 1, 8))
			return false;
	} 
	else
	{
		Assert(lastBlockFlag != BITMAP_COMPRESSION_FLAG_RLE && lastBlockFlag != BITMAP_COMPRESSION_FLAG_RAW);
		/* If the number of repeated is not high enough,
		 * it is better to write the encoded one/zeros out
		 */
		for (i = 0; i < rleRepeatCount; i++)
		{
			if (!Bitstream_Put(bitstream, lastBlockFlag, 2))
				return false;
		}
	}
	return true;
}

static bool 
Bitmap_Compress_DefaultCompress(
		uint32* bitmap,
		int blockCount,
		Bitstream *bitstream)
{
	BitmapCompressBlockController compBlockCtl = {0};
	compBlockCtl.bitstream = bitstream;
	compBlockCtl.isFirstBlock = true;

	for (int i = 0; i < blockCount; i++)
	{
		compBlockCtl.blockData = bitmap[i];
		if(!Bitmap_CompressBlock(&compBlockCtl))
		{
			return false;
		}
	}

	/* Write last RLE block */
	if (compBlockCtl.rleRepeatCount > 0)
	{
		if (!Bitmap_EncodeRLE(bitstream, compBlockCtl.rleRepeatCount,
					compBlockCtl.lastBlockFlag))
			return false;
	}
	return true;
}

static bool
Bitmap_Compress_Write_Header(BitmapCompressionType compressionType,
		int blockCount, Bitstream *bitstream)
{
	if(!Bitstream_Put(bitstream, compressionType, 1))
		return false;
	if(!Bitstream_Skip(bitstream, 3))
		return false;
	if (!Bitstream_Put(bitstream, blockCount, 12))
		return false;
	Assert(Bitstream_GetOffset(bitstream) == 16U);
	return true;
}

/*
 * Compresses the given bitmap data.
 * 
 * blockCount in uint32-words.
 *
 */ 
int
Bitmap_Compress(
		BitmapCompressionType	compressionType,
		uint32					*bitmap,
		int						blockCount,
		unsigned char			*outData,
		int						maxOutDataSize)
{
	Bitstream bitstream;

	Assert(maxOutDataSize >= (blockCount * sizeof(uint32) + 2));

	memset(outData, 0, maxOutDataSize);

	/* Header 
	 */
	Bitstream_Init(&bitstream, outData, maxOutDataSize);
	if (!Bitmap_Compress_Write_Header(compressionType,blockCount, &bitstream))
		elog(ERROR, "Failed to write bitmap compression header");
	/* bitmap content */
	switch (compressionType)
	{
		case BITMAP_COMPRESSION_TYPE_NO:
			return Bitmap_Compress_NoCompress(bitmap,
											  blockCount,
											  &bitstream);
		case BITMAP_COMPRESSION_TYPE_DEFAULT:
			if (!Bitmap_Compress_DefaultCompress(bitmap, blockCount,
						&bitstream))
			{
				/* This may happen when the input bitmap is not nicely compressible */
				/* Fall back */
				memset(outData, 0, maxOutDataSize);
				return Bitmap_Compress(
						BITMAP_COMPRESSION_TYPE_NO,
						bitmap,
						blockCount,
						outData,
						maxOutDataSize);
			}
			else
			{
				return Bitstream_GetLength(&bitstream);
			}
		default:
			elog(ERROR, "illegal compression type during bitmap compression: "
				"compression type %d", compressionType);
			return 0;
	}
}

/* Compress a particular bitmap block */
static bool
Bitmap_CompressBlock(BitmapCompressBlockController *compBlockCtl)
{
	/*
	 * When
	 *  1. current block equals to previous block
	 *  2. repeat count <= 255
	 *  3. current block is not the first block
	 * We just increase the repeat count.
	 */
	if (compBlockCtl->blockData == compBlockCtl->lastBlockData
		&& compBlockCtl->rleRepeatCount <= 255
		&& !compBlockCtl->isFirstBlock)
	{
		(compBlockCtl->rleRepeatCount)++;
	}
	else
	{
		compBlockCtl->isFirstBlock = false;
		/* Write the repeat code */
		if (compBlockCtl->rleRepeatCount > 0)
		{
			if (!Bitmap_EncodeRLE(compBlockCtl->bitstream,
								  compBlockCtl->rleRepeatCount,
								  compBlockCtl->lastBlockFlag))
			{
				return false;
			}
			compBlockCtl->rleRepeatCount = 0;
		}

		/* Write the ZERO code */
		if (compBlockCtl->blockData == 0)
		{
			if (!Bitstream_Put(compBlockCtl->bitstream,
							   BITMAP_COMPRESSION_FLAG_ZERO,
							   2))
			{
				return false;
			}
			compBlockCtl->lastBlockFlag = BITMAP_COMPRESSION_FLAG_ZERO;
		}
		/* Write the ONE code */
		else if (compBlockCtl->blockData == 0xFFFFFFFFU)
		{
			if (!Bitstream_Put(compBlockCtl->bitstream,
							   BITMAP_COMPRESSION_FLAG_ONE,
							   2))
			{
				return false;
			}
			compBlockCtl->lastBlockFlag = BITMAP_COMPRESSION_FLAG_ONE;
		}
		/* Write the RAW code */
		else
		{
			if (!Bitstream_Put(compBlockCtl->bitstream,
							   BITMAP_COMPRESSION_FLAG_RAW,
							   2))
			{
				return false;
			}
			if (!Bitstream_Put(compBlockCtl->bitstream,
							   compBlockCtl->blockData,
							   32))
			{
				return false;
			}
			compBlockCtl->lastBlockFlag = BITMAP_COMPRESSION_FLAG_RAW;
		}

		compBlockCtl->lastBlockData = compBlockCtl->blockData;
	}
	return true;
}

/*
 * Compress bitmap by BITMAP_COMPRESSION_TYPE_NO method
 * (no actual compression, just copy data)
 */
static int
Bitmap_Compress_NoCompress(
		uint32*		bitmap,
		int			blockCount,
		Bitstream	*bitstream)
{
	unsigned char *offset;
	int bitStreamLen;

	if (blockCount == 0)
	{
		/* we only have the header */
		return 2;
	}

	offset = Bitstream_GetAlignedData(bitstream, 16);
	memcpy(offset,
		   bitmap,
		   blockCount * sizeof(uint32));

	bitStreamLen = (blockCount * sizeof(uint32)) + 2;
	/*
	 * TODO:
	 * The assertion failed because we directly wrote the bit stream but did not
	 * update the offset. Maybe we can refactor the code by a more telegant way
	 * in future.
	 */
	/* Assert(bitStreamLen == Bitstream_GetLength(bitstream)); */

	return bitStreamLen;
}

/*
 * Decompress bitmap by BITMAP_COMPRESSION_TYPE_DEFAULT method
 */
static void
Bitmap_Compress_DefaultDecompress(BitmapDecompressState *state,
								  uint32 *bitmap)
{
	uint32	lastBlockData = 0;
	uint32	rleRepeatCount = 0;
	uint32	flag = 0;
	bool	failed = false;
	uint32	*nextPos = 0;

	for (int i = 0; i < state->blockCount; i++)
	{
		nextPos = bitmap + i;

		if (rleRepeatCount == 0)
		{
			if (!Bitstream_Get(&state->bitstream, 2, &flag))
			{
				failed = true;
				break;
			}
			switch (flag)
			{
				case BITMAP_COMPRESSION_FLAG_ZERO:
					*nextPos = 0;
					break;
				case BITMAP_COMPRESSION_FLAG_ONE:
					*nextPos = 0xFFFFFFFF;
					break;
				case BITMAP_COMPRESSION_FLAG_RAW:
					if (!Bitstream_Get(&state->bitstream, 32, nextPos))
					{
						failed = true;
						break;
					}
					break;
				case BITMAP_COMPRESSION_FLAG_RLE:
					Assert(i != 0);
					if (!Bitstream_Get(&state->bitstream, 8, &rleRepeatCount))
					{
						failed = true;
						break;
					}
					*nextPos  = lastBlockData;
					break;
				default:
					elog(ERROR, "Invalid compression flag");
			}
			lastBlockData = *nextPos;
		}
		else
		{
			/* In an RLE block */
			*nextPos = lastBlockData;
			rleRepeatCount--;
		}
	}
	if (rleRepeatCount > 0)
	{
		elog(ERROR, "illegal RLE state after bitmap decompression: "
					"block count %d, compression type %d, rle repeat count %u",
			 state->blockCount, state->compressionType, rleRepeatCount);
	}

	if (failed)
	{
		elog(ERROR, "bitstream read error seen during decompression: "
					"block count %d, compression type %d",
		state->blockCount, state->compressionType);
	}
}

/*
 * Decompress bitmap by BITMAP_COMPRESSION_TYPE_NO method
 * (no actual compression, just copy data)
 */
static void
Bitmap_Compress_DecompressNone(BitmapDecompressState *state,
							   uint32 *bitmap)
{
	/* Work for both 64bit and 32bit word */
	memcpy(bitmap,
		   Bitstream_GetAlignedData(&state->bitstream, 16), 
		   state->blockCount * sizeof(uint32));
}
