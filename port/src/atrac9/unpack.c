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

	return ERR_SUCCESS;
}

static At9Status UnpackBlock(Block* block, BitReaderCxt* br)
{
	ERROR_CHECK(ReadBlockHeader(block, br));

	if (block->BlockType == LFE)
	{
		ERROR_CHECK(UnpackLfeBlock(block, br));
	}
	else
	{
		ERROR_CHECK(UnpackStandardBlock(block, br));
	}

	AlignPosition(br, 8);
	return ERR_SUCCESS;
}

static At9Status ReadBlockHeader(Block* block, BitReaderCxt* br)
{
	block->FirstInSuperframe = !ReadInt(br, 1);
	block->ReuseBandParams = ReadInt(br, 1);

	if (block->FirstInSuperframe && block->ReuseBandParams && block->BlockType != LFE)
	{
		return ERR_UNPACK_REUSE_BAND_PARAMS_INVALID;
	}

	return ERR_SUCCESS;
}

static At9Status UnpackStandardBlock(Block* block, BitReaderCxt* br)
{
	if (!block->ReuseBandParams)
	{
		ERROR_CHECK(ReadBandParams(block, br));
	}

	ERROR_CHECK(ReadGradientParams(block, br));
	ERROR_CHECK(CreateGradient(block));
	ERROR_CHECK(ReadStereoParams(block, br));
	ERROR_CHECK(ReadExtensionParams(block, br));

	for (int i = 0; i < block->ChannelCount; i++)
	{
		Channel* channel = &block->Channels[i];
		UpdateCodedUnits(channel);

		ERROR_CHECK(ReadScaleFactors(channel, br));
		CalculateMask(channel);
		CalculatePrecisions(channel);
		CalculateSpectrumCodebookIndex(channel);

		ERROR_CHECK(ReadSpectra(channel, br));
		ERROR_CHECK(ReadSpectraFine(channel, br));
	}

	block->QuantizationUnitsPrev = block->BandExtensionEnabled ? block->ExtensionUnit : block->QuantizationUnitCount;
	return ERR_SUCCESS;
}

static At9Status ReadBandParams(Block* block, BitReaderCxt* br)
{
	const int minBandCount = MinBandCount[block->Config->HighSampleRate];
	const int maxExtensionBand = MaxExtensionBand[block->Config->HighSampleRate];
	block->BandCount = ReadInt(br, 4) + minBandCount;
	block->QuantizationUnitCount = BandToQuantUnitCount[block->BandCount];

	if (block->BandCount > MaxBandCount[block->Config->SampleRateIndex])
	{
		return ERR_UNPACK_BAND_PARAMS_INVALID;
	}

	if (block->BlockType == Stereo)
	{
		block->StereoBand = ReadInt(br, 4);
		block->StereoBand += minBandCount;
		block->StereoQuantizationUnit = BandToQuantUnitCount[block->StereoBand];
	}
	else
	{
		block->StereoBand = block->BandCount;
	}

	if (block->StereoBand > block->BandCount)
	{
		return ERR_UNPACK_BAND_PARAMS_INVALID;
	}

	block->BandExtensionEnabled = ReadInt(br, 1);
	if (block->BandExtensionEnabled)
	{
		block->ExtensionBand = ReadInt(br, 4);
		block->ExtensionBand += minBandCount;

		if (block->ExtensionBand < block->BandCount || block->ExtensionBand > maxExtensionBand)
		{
			return ERR_UNPACK_BAND_PARAMS_INVALID;
		}

		block->ExtensionUnit = BandToQuantUnitCount[block->ExtensionBand];
	}
	else
	{
		block->ExtensionBand = block->BandCount;
		block->ExtensionUnit = block->QuantizationUnitCount;
	}

	return ERR_SUCCESS;
}

static At9Status ReadGradientParams(Block* block, BitReaderCxt* br)
{
	block->GradientMode = ReadInt(br, 2);
	if (block->GradientMode > 0)
	{
		block->GradientEndUnit = 31;
		block->GradientEndValue = 31;
		block->GradientStartUnit = ReadInt(br, 5);
		block->GradientStartValue = ReadInt(br, 5);
	}
	else
	{
		block->GradientStartUnit = ReadInt(br, 6);
		block->GradientEndUnit = ReadInt(br, 6) + 1;
		block->GradientStartValue = ReadInt(br, 5);
		block->GradientEndValue = ReadInt(br, 5);
	}
	block->GradientBoundary = ReadInt(br, 4);

	if (block->GradientBoundary > block->QuantizationUnitCount)
	{
		return ERR_UNPACK_GRAD_BOUNDARY_INVALID;
	}
	if (block->GradientStartUnit < 0 || block->GradientStartUnit >= 48)
	{
		return ERR_UNPACK_GRAD_START_UNIT_OOB;
	}
	if (block->GradientEndUnit < 0 || block->GradientEndUnit >= 48)
	{
		return ERR_UNPACK_GRAD_END_UNIT_OOB;
	}
	if (block->GradientStartUnit > block->GradientEndUnit)
	{
		return ERR_UNPACK_GRAD_END_UNIT_INVALID;
	}
	if (block->GradientStartValue < 0 || block->GradientStartValue >= 32)
	{
		return ERR_UNPACK_GRAD_START_VALUE_OOB;
	}
	if (block->GradientEndValue < 0 || block->GradientEndValue >= 32)
	{
		return ERR_UNPACK_GRAD_END_VALUE_OOB;
	}

	return ERR_SUCCESS;
}

static At9Status ReadStereoParams(Block* block, BitReaderCxt* br)
{
	if (block->BlockType != Stereo) return ERR_SUCCESS;

	block->PrimaryChannelIndex = ReadInt(br, 1);
	block->HasJointStereoSigns = ReadInt(br, 1);
	if (block->HasJointStereoSigns)
	{
		for (int i = block->StereoQuantizationUnit; i < block->QuantizationUnitCount; i++)
		{
			block->JointStereoSigns[i] = ReadInt(br, 1);
		}
	}
	else
	{
		memset(block->JointStereoSigns, 0, sizeof(block->JointStereoSigns));
	}

	return ERR_SUCCESS;
}

static void BexReadHeader(Channel* channel, BitReaderCxt* br, int bexBand)
{
	const int bexMode = ReadInt(br, 2);
	channel->BexMode = bexBand > 2 ? bexMode : 4;
	channel->BexValueCount = BexEncodedValueCounts[channel->BexMode][bexBand];
}

static void BexReadData(Channel* channel, BitReaderCxt* br, int bexBand)
{
	for (int i = 0; i < channel->BexValueCount; i++)
	{
