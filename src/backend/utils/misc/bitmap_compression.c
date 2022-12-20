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

typedef struct BitmapCompressBlockData
{
	Bitstream *bitstream;
	uint32 blockData;
	bool isFirstBlock;
	uint32 lastBlockData;
	int lastBlockFlag;
	int rleRepeatCount;
} BitmapCompressBlockData;

static bool
Bitmap_CompressBlock(BitmapCompressBlockData *compBlockData);

static int
Bitmap_Compress_NoCompress(
		uint32* bitmap,
		int blockCount,
		bool isOnly32bitOneWord,
		Bitstream *bitstream);

static void
Bitmap_Compress_Decompress(BitmapDecompressState *state,
						   uint32 *bitmap);

static void
Bitmap_Compress_NoDecompress(BitmapDecompressState *state,
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
		Bitmap_Compress_NoDecompress(state, bitmap);
	}
	else if (state->compressionType == BITMAP_COMPRESSION_TYPE_DEFAULT)
	{
		Bitmap_Compress_Decompress(state, bitmap);
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
Bitmap_Compress_Default(
		uint32* bitmap,
		int blockCount,
		bool isOnly32bitOneWord,
		Bitstream *bitstream)
{
	BitmapCompressBlockData compBlockData = {0};
	compBlockData.bitstream = bitstream;
	compBlockData.isFirstBlock = true;

	if (BITS_PER_BITMAPWORD == 32)
	{
		for (int i = 0; i < blockCount; i++)
		{
			compBlockData.blockData = bitmap[i];
			if(!Bitmap_CompressBlock(&compBlockData))
			{
				return false;
			}
		}
	}
	else
	{
		Assert(BITS_PER_BITMAPWORD == 64);
		/* For 64bit bms blockCount must be 0 or even number */
		Assert(blockCount % 2 == 0);

#ifdef WORDS_BIGENDIAN

		/*
		 * On a big-endian env, we need to compress the blocks in a interlaced
		 * order, i.e.: 2,1,4,3,6,5,...
		 */
		for (int i = 0;i < blockCount; i += 2)
		{
			compBlockData.blockData = bitmap[i + 1];
			if(!Bitmap_CompressBlock(&compBlockData))
			{
				return false;
			}

			/*
			 * If there is only one 32bit word, skip the latest word
			 */
			if (isOnly32bitOneWord)
			{
				Assert(blockCount == 2);
				Assert(bitmap[0] == 0);
				break;
			}

			compBlockData.blockData = bitmap[i];
			if(!Bitmap_CompressBlock(&compBlockData))
			{
				return false;
			}
		}

#else

		for (int i = 0; i < blockCount; i++)
		{
			compBlockData.blockData = bitmap[i];
			if(!Bitmap_CompressBlock(&compBlockData))
			{
				return false;
			}

			/*
			 * If there is only one 32bit word, skip the latest word
			 */
			if (isOnly32bitOneWord)
			{
				Assert(blockCount == 2);
				Assert(bitmap[1] == 0);
				break;
			}
		}

#endif

	}

	/* Write last RLE block */
	if (compBlockData.rleRepeatCount > 0)
	{
		if (!Bitmap_EncodeRLE(bitstream, compBlockData.rleRepeatCount,
					compBlockData.lastBlockFlag))
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
 * Note: to keep consistency with (ondisk) bitstream in 32bit words, we need
 * to generate 32bit bitstream even on 64bit env, i.e. generating bitstream
 * in 32bit words by 64bit bitmapset.
 */ 
int
Bitmap_Compress(
		BitmapCompressionType compressionType,
		uint32* bitmap,
		int blockCount,
		unsigned char *outData,
		int maxOutDataSize,
		bool isOnly32bitOneWord)
{
	Bitstream bitstream;
	/*
	 * onDiskBlockCount is the block count of (ondisk) bitstream. In most cases
	 * it is the same to blockCount (the block count of in-memory bms) because
	 * both are in uint32 words. However, there is a special case:
	 * On 64bit env, when there is only one 32bit word ondisk, blockCount should
	 * be 2 since a 64bit word always has two 32bit word. We need to set
	 * onDiskBlockCount = 1 for the case.
	 */
	int onDiskBlockCount = blockCount;

	if (isOnly32bitOneWord)
	{
		Assert(BITS_PER_BITMAPWORD == 64);
		Assert(maxOutDataSize == sizeof(uint32) + 2);
		Assert(blockCount == 2);

		onDiskBlockCount = 1;
	}
	else
	{
		Assert(maxOutDataSize >= (blockCount * sizeof(uint32) + 2));
	}

	memset(outData, 0, maxOutDataSize);

	/* Header 
	 */
	Bitstream_Init(&bitstream, outData, maxOutDataSize);
	if (!Bitmap_Compress_Write_Header(compressionType,
									  onDiskBlockCount,
									  &bitstream))
		elog(ERROR, "Failed to write bitmap compression header");

	/* bitmap content */
	switch (compressionType)
	{
		case BITMAP_COMPRESSION_TYPE_NO:
			return Bitmap_Compress_NoCompress(bitmap,
											  blockCount,
											  isOnly32bitOneWord,
											  &bitstream);
		case BITMAP_COMPRESSION_TYPE_DEFAULT:
			if (!Bitmap_Compress_Default(bitmap, blockCount, isOnly32bitOneWord,
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
						maxOutDataSize,
						isOnly32bitOneWord);
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

static bool
Bitmap_CompressBlock(BitmapCompressBlockData *compBlockData)
{
	if (compBlockData->blockData == compBlockData->lastBlockData
		&& compBlockData->rleRepeatCount <= 255
		&& !compBlockData->isFirstBlock)
	{
		(compBlockData->rleRepeatCount)++;
	}
	else
	{
		compBlockData->isFirstBlock = false;
		if (compBlockData->rleRepeatCount > 0)
		{
			if (!Bitmap_EncodeRLE(compBlockData->bitstream,
								  compBlockData->rleRepeatCount,
								  compBlockData->lastBlockFlag))
			{
				return false;
			}
			compBlockData->rleRepeatCount = 0;
		}

		if (compBlockData->blockData == 0)
		{
			if (!Bitstream_Put(compBlockData->bitstream,
							   BITMAP_COMPRESSION_FLAG_ZERO,
							   2))
			{
				return false;
			}
			compBlockData->lastBlockFlag = BITMAP_COMPRESSION_FLAG_ZERO;
		}
		else if (compBlockData->blockData == 0xFFFFFFFFU)
		{
			if (!Bitstream_Put(compBlockData->bitstream,
							   BITMAP_COMPRESSION_FLAG_ONE,
							   2))
			{
				return false;
			}
			compBlockData->lastBlockFlag = BITMAP_COMPRESSION_FLAG_ONE;
		}
		else
		{
			if (!Bitstream_Put(compBlockData->bitstream,
							   BITMAP_COMPRESSION_FLAG_RAW,
							   2))
			{
				return false;
			}
			if (!Bitstream_Put(compBlockData->bitstream,
							   compBlockData->blockData,
							   32))
			{
				return false;
			}
			compBlockData->lastBlockFlag = BITMAP_COMPRESSION_FLAG_RAW;
		}

		compBlockData->lastBlockData = compBlockData->blockData;
	}
	return true;
}

static int
Bitmap_Compress_NoCompress(
		uint32* bitmap,
		int blockCount,
		bool isOnly32bitOneWord,
		Bitstream *bitstream)
{
	unsigned char *offset = 0;
	int bitStreamLen = 0;

	// By assertion I know that I have sufficient space for this
	if (blockCount == 0)
	{
		/* we only have the header */
		return 2;
	}

	offset = Bitstream_GetAlignedData(bitstream, 16);
	if (BITS_PER_BITMAPWORD == 32)
	{
		memcpy(offset,
			   bitmap,
			   blockCount * sizeof(uint32));
	}
	else
	{
#ifdef WORDS_BIGENDIAN
		for (int i = 0; i < blockCount; i += 2)
		{
			memcpy(offset,
				   bitmap[i + 1],
				   sizeof(uint32));
			offset += sizeof(uint32);

			if (isOnly32bitOneWord)
				break;
			memcpy(offset,
				   bitmap[i],
				   sizeof(uint32));
			offset += sizeof(uint32);
		}
#else
		memcpy(offset,
			   bitmap,
			   (isOnly32bitOneWord ? 1 : blockCount) * sizeof(uint32));
#endif
	}

	bitStreamLen = ((isOnly32bitOneWord ? 1 : blockCount) * sizeof(uint32)) + 2;
	//Assert(bitStreamLen == Bitstream_GetLength(bitstream));

	return bitStreamLen;
}

static void
Bitmap_Compress_Decompress(BitmapDecompressState *state,
						   uint32 *bitmap)
{
	uint32 lastBlockData = 0;
	uint32 rleRepeatCount = 0;
	uint32 flag = 0;
	bool failed = false;
	uint32 *nextPos = 0;

	for (int i = 0; i < state->blockCount; i++)
	{
#ifdef WORDS_BIGENDIAN
		if (BITS_PER_BITMAPWORD == 64)
		{
			if(i % 2 == 0)
			{
				nextPos = bitmap + i + 1;
			}
			else
			{
				nextPos = bitmap + i - 1;
			}
		}
#else
		nextPos = bitmap + i;
#endif
			
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

static void
Bitmap_Compress_NoDecompress(BitmapDecompressState *state,
							 uint32 *bitmap)
{
	if (BITS_PER_BITMAPWORD == 32)
	{
		memcpy(bitmap, 
			   Bitstream_GetAlignedData(&state->bitstream, 16), 
			   state->blockCount * sizeof(uint32));
	}
	else
	{
#ifdef WORDS_BIGENDIAN
		/*
		 * If we are using 64bit bms and it is a big-endian system, read the
		 * data from bit stream and copy them to bms by an interlay order.
		 */
		uint32 *offset = Bitstream_GetAlignedData(&state->bitstream, 16);
		for (int i = 0; i < state->blockCount; i++)
		{
			bitmap[i + 1] = *(offset + 1);
			/* If there is only one block, skip the rest. */
			if (state->blockCount == 1)
			{
				break;
			}
			bitmap[i] = *(offset);
			offset += 2;
		}
#else
		memcpy(bitmap, 
			   Bitstream_GetAlignedData(&state->bitstream, 16), 
			   state->blockCount * sizeof(uint32));
#endif
	}
}
