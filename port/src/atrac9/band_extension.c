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

	const int groupABin = QuantUnitToCoeffIndex[groupAUnit];
	const int groupBBin = QuantUnitToCoeffIndex[groupBUnit];
	const int groupCBin = QuantUnitToCoeffIndex[groupCUnit];
	const int totalBins = QuantUnitToCoeffIndex[totalUnits];

	FillHighFrequencies(spectra, groupABin, groupBBin, groupCBin, totalBins);

	double groupAScale, groupBScale, groupCScale;
	double rate, scale, mult;

	switch (channel->BexMode)
	{
	case 0:
		switch (bandCount)
		{
		case 3:
			scales[0] = BexMode0Bands3[0][values[0]];
			scales[1] = BexMode0Bands3[1][values[0]];
			scales[2] = BexMode0Bands3[2][values[1]];
			scales[3] = BexMode0Bands3[3][values[2]];
			scales[4] = BexMode0Bands3[4][values[3]];
			break;
		case 4:
			scales[0] = BexMode0Bands4[0][values[0]];
			scales[1] = BexMode0Bands4[1][values[0]];
			scales[2] = BexMode0Bands4[2][values[1]];
			scales[3] = BexMode0Bands4[3][values[2]];
			scales[4] = BexMode0Bands4[4][values[3]];
			break;
		case 5:
			scales[0] = BexMode0Bands5[0][values[0]];
			scales[1] = BexMode0Bands5[1][values[1]];
			scales[2] = BexMode0Bands5[2][values[1]];
			break;
		}

		scales[bexQuantUnits - 1] = SpectrumScale[scaleFactors[groupAUnit]];

		AddNoiseToSpectrum(channel, QuantUnitToCoeffIndex[totalUnits - 1],
			QuantUnitToCoeffCount[totalUnits - 1]);
		ScaleBexQuantUnits(spectra, scales, groupAUnit, totalUnits);
		break;
	case 1:
		for (int i = groupAUnit; i < totalUnits; i++)
		{
			scales[i - groupAUnit] = SpectrumScale[scaleFactors[i]];
		}

		AddNoiseToSpectrum(channel, groupABin, totalBins - groupABin);
		ScaleBexQuantUnits(spectra, scales, groupAUnit, totalUnits);
		break;
	case 2:
		groupAScale = BexMode2Scale[values[0]];
		groupBScale = BexMode2Scale[values[1]];

		for (int i = groupABin; i < groupBBin; i++)
		{
			spectra[i] *= groupAScale;
		}

		for (int i = groupBBin; i < groupCBin; i++)
		{
			spectra[i] *= groupBScale;
		}
		return;
	case 3:
		rate = pow(2, BexMode3Rate[values[1]]);
		scale = BexMode3Initial[values[0]];
		for (int i = groupABin; i < totalBins; i++)
		{
			scale *= rate;
			spectra[i] *= scale;
		}
		return;
	case 4:
		mult = BexMode4Multiplier[values[0]];
		groupAScale = 0.7079468 * mult;
		groupBScale = 0.5011902 * mult;
		groupCScale = 0.3548279 * mult;

		for (int i = groupABin; i < groupBBin; i++)
		{
			spectra[i] *= groupAScale;
		}

		for (int i = groupBBin; i < groupCBin; i++)
		{
			spectra[i] *= groupBScale;
		}

		for (int i = groupCBin; i < totalBins; i++)
		{
			spectra[i] *= groupCScale;
		}
	}
}
