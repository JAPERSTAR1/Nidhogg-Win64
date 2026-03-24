#include "scale_factors.h"
#include "huffCodes.h"
#include "tables.h"
#include "utility.h"
#include <string.h>

static void ReadClcOffset(Channel* channel, BitReaderCxt* br);
static void ReadVlcDeltaOffset(Channel* channel, BitReaderCxt* br);
static void ReadVlcDistanceToBaseline(Channel* channel, BitReaderCxt* br, const int* baseline, const int baselineLength);
static void ReadVlcDeltaOffsetWithBaseline(Channel* channel, BitReaderCxt* br, const int* baseline, const int baselineLength);

At9Status ReadScaleFactors(Channel * channel, BitReaderCxt * br)
{
	memset(channel->ScaleFactors, 0, sizeof(channel->ScaleFactors));

	channel->ScaleFactorCodingMode = ReadInt(br, 2);
	if (channel->ChannelIndex == 0)
	{
		switch (channel->ScaleFactorCodingMode)
		{
		case 0:
			ReadVlcDeltaOffset(channel, br);
			break;
		case 1:
			ReadClcOffset(channel, br);
			break;
		case 2:
			if (channel->Block->FirstInSuperframe) return ERR_UNPACK_SCALE_FACTOR_MODE_INVALID;
			ReadVlcDistanceToBaseline(channel, br, channel->ScaleFactorsPrev, channel->Block->QuantizationUnitsPrev);
			break;
		case 3:
			if (channel->Block->FirstInSuperframe) return ERR_UNPACK_SCALE_FACTOR_MODE_INVALID;
			ReadVlcDeltaOffsetWithBaseline(channel, br, channel->ScaleFactorsPrev, channel->Block->QuantizationUnitsPrev);
			break;
		}
	}
	else
	{
		switch (channel->ScaleFactorCodingMode)
		{
		case 0:
			ReadVlcDeltaOffset(channel, br);
			break;
		case 1:
			ReadVlcDistanceToBaseline(channel, br, channel->Block->Channels[0].ScaleFactors, channel->Block->ExtensionUnit);
			break;
		case 2:
			ReadVlcDeltaOffsetWithBaseline(channel, br, channel->Block->Channels[0].ScaleFactors, channel->Block->ExtensionUnit);
