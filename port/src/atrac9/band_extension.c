#include "band_extension.h"
#include "tables.h"
#include "utility.h"
#include <math.h>

static void ApplyBandExtensionChannel(Channel* channel);

static void ScaleBexQuantUnits(double* spectra, double* scales, int startUnit, int totalUnits);
static void FillHighFrequencies(double* spectra, int groupABin, int groupBBin, int groupCBin, int totalBins);
static void AddNoiseToSpectrum(Channel* channel, int index, int count);

static void RngInit(RngCxt* rng, unsigned short seed);
static unsigned short RngNext(RngCxt* rng);

static const double BexMode0Bands3[5][32];
static const double BexMode0Bands4[5][16];
static const double BexMode0Bands5[3][32];
static const double BexMode2Scale[64];
static const double BexMode3Initial[16];
static const double BexMode3Rate[16];
static const double BexMode4Multiplier[8];

void ApplyBandExtension(Block* block)
{
	if (!block->BandExtensionEnabled || !block->HasExtensionData) return;

	for (int i = 0; i < block->ChannelCount; i++)
	{
		ApplyBandExtensionChannel(&block->Channels[i]);
	}
}

static void ApplyBandExtensionChannel(Channel* channel)
{
	const int groupAUnit = channel->Block->QuantizationUnitCount;
	int* scaleFactors = channel->ScaleFactors;
	double* spectra = channel->Spectra;
	double scales[6];
	int* values = channel->BexValues;

	const BexGroup* bexInfo = &BexGroupInfo[channel->Block->QuantizationUnitCount - 13];
	const int bandCount = bexInfo->BandCount;
	const int groupBUnit = bexInfo->GroupBUnit;
	const int groupCUnit = bexInfo->GroupCUnit;

	const int totalUnits = Max(groupCUnit, 22);
	const int bexQuantUnits = totalUnits - groupAUnit;

