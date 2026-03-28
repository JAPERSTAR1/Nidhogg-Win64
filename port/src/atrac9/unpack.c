#include "unpack.h"
#include "band_extension.h"
#include "bit_allocation.h"
#include "huffCodes.h"
#include "scale_factors.h"
#include "tables.h"
#include "utility.h"
#include <string.h>

static At9Status UnpackBlock(Block* block, BitReaderCxt* br);
static At9Status ReadBlockHeader(Block* block, BitReaderCxt* br);
static At9Status UnpackStandardBlock(Block* block, BitReaderCxt* br);
static At9Status ReadBandParams(Block* block, BitReaderCxt* br);
static At9Status ReadGradientParams(Block* block, BitReaderCxt* br);
static At9Status ReadStereoParams(Block* block, BitReaderCxt* br);
static At9Status ReadExtensionParams(Block* block, BitReaderCxt* br);
static void UpdateCodedUnits(Channel* channel);
static void CalculateSpectrumCodebookIndex(Channel* channel);

static At9Status ReadSpectra(Channel* channel, BitReaderCxt* br);
static At9Status ReadSpectraFine(Channel* channel, BitReaderCxt* br);

static At9Status UnpackLfeBlock(Block* block, BitReaderCxt* br);
static void DecodeLfeScaleFactors(Channel* channel, BitReaderCxt* br);
static void CalculateLfePrecision(Channel* channel);
static void ReadLfeSpectra(Channel* channel, BitReaderCxt* br);

At9Status UnpackFrame(Frame* frame, BitReaderCxt* br)
{
	const int blockCount = frame->Config->ChannelConfig.BlockCount;

	for (int i = 0; i < blockCount; i++)
	{
		ERROR_CHECK(UnpackBlock(&frame->Blocks[i], br));

		if (frame->Blocks[i].FirstInSuperframe && frame->IndexInSuperframe)
		{
			return ERR_UNPACK_SUPERFRAME_FLAG_INVALID;
		}
	}

	frame->IndexInSuperframe++;

	if (frame->IndexInSuperframe == frame->Config->FramesPerSuperframe)
	{
		frame->IndexInSuperframe = 0;
	}

