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

static void ScaleBexQuantUnits(double* spectra, double* scales, int startUnit, int totalUnits)
{
	for (int i = startUnit; i < totalUnits; i++)
	{
		for (int k = QuantUnitToCoeffIndex[i]; k < QuantUnitToCoeffIndex[i + 1]; k++)
		{
			spectra[k] *= scales[i - startUnit];
		}
	}
}

static void FillHighFrequencies(double* spectra, int groupABin, int groupBBin, int groupCBin, int totalBins)
{
	for (int i = 0; i < groupBBin - groupABin; i++)
	{
		spectra[groupABin + i] = spectra[groupABin - i - 1];
	}

	for (int i = 0; i < groupCBin - groupBBin; i++)
	{
		spectra[groupBBin + i] = spectra[groupBBin - i - 1];
	}

	for (int i = 0; i < totalBins - groupCBin; i++)
	{
		spectra[groupCBin + i] = spectra[groupCBin - i - 1];
	}
}

static void AddNoiseToSpectrum(Channel* channel, int index, int count)
{
	if (!channel->Rng.Initialized)
	{
		int* sf = channel->ScaleFactors;
		const unsigned short seed = (unsigned short)(543 * (sf[8] + sf[12] + sf[15] + 1));
		RngInit(&channel->Rng, seed);
	}
	for (int i = 0; i < count; i++)
	{
		channel->Spectra[i + index] = RngNext(&channel->Rng) / 65535.0 * 2.0 - 1.0;
	}
}

static void RngInit(RngCxt* rng, unsigned short seed)
{
	const int startValue = 0x4D93 * (seed ^ (seed >> 14));

	rng->StateA = (unsigned short)(3 - startValue);
	rng->StateB = (unsigned short)(2 - startValue);
	rng->StateC = (unsigned short)(1 - startValue);
	rng->StateD = (unsigned short)(0 - startValue);
	rng->Initialized = TRUE;
}

static unsigned short RngNext(RngCxt* rng)
{
	const unsigned short t = (unsigned short)(rng->StateD ^ (rng->StateD << 5));
	rng->StateD = rng->StateC;
	rng->StateC = rng->StateB;
	rng->StateB = rng->StateA;
	rng->StateA = (unsigned short)(t ^ rng->StateA ^ ((t ^ (rng->StateA >> 5)) >> 4));
	return rng->StateA;
}

const BexGroup BexGroupInfo[8] =
{
	{ 16, 21, 0 },
	{ 18, 22, 1 },
	{ 20, 22, 2 },
	{ 21, 22, 3 },
	{ 21, 22, 3 },
	{ 23, 24, 4 },
	{ 23, 24, 4 },
	{ 24, 24, 5 }
};

const char BexEncodedValueCounts[5][6] =
{
	{0, 0, 0, 4, 4, 2},
	{0, 0, 0, 0, 0, 0},
	{0, 0, 0, 2, 2, 1},
	{0, 0, 0, 2, 2, 2},
	{1, 1, 1, 0, 0, 0}
};

// [mode][bands][valueIndex]
const char BexDataLengths[5][6][4] =
{
	{
		{0, 0, 0, 0},
		{0, 0, 0, 0},
		{0, 0, 0, 0},
		{5, 4, 3, 3},
		{4, 4, 3, 4},
		{4, 5, 0, 0}
	}, {
		{0, 0, 0, 0},
		{0, 0, 0, 0},
		{0, 0, 0, 0},
		{0, 0, 0, 0},
		{0, 0, 0, 0},
		{0, 0, 0, 0}
	}, {
		{0, 0, 0, 0},
		{0, 0, 0, 0},
		{0, 0, 0, 0},
		{6, 6, 0, 0},
		{6, 6, 0, 0},
		{6, 0, 0, 0}
	}, {
		{0, 0, 0, 0},
		{0, 0, 0, 0},
		{0, 0, 0, 0},
		{4, 4, 0, 0},
		{4, 4, 0, 0},
		{4, 4, 0, 0}
	}, {
		{3, 0, 0, 0},
		{3, 0, 0, 0},
		{3, 0, 0, 0},
		{0, 0, 0, 0},
		{0, 0, 0, 0},
		{0, 0, 0, 0}
	}
};

static const double BexMode0Bands3[5][32] =
{
	{
		0.000000e+0, 1.988220e-1, 2.514343e-1, 2.960510e-1,
		3.263550e-1, 3.771362e-1, 3.786926e-1, 4.540405e-1,
		4.877625e-1, 5.262451e-1, 5.447083e-1, 5.737000e-1,
		6.212158e-1, 6.222839e-1, 6.560974e-1, 6.896667e-1,
		7.555542e-1, 7.677917e-1, 7.918091e-1, 7.971497e-1,
		8.188171e-1, 8.446045e-1, 9.790649e-1, 9.822083e-1,
		9.846191e-1, 9.859314e-1, 9.863586e-1, 9.863892e-1,
		9.873352e-1, 9.881287e-1, 9.898682e-1, 9.913330e-1
	}, {
		0.000000e+0, 9.982910e-1, 7.592773e-2, 7.179565e-1,
		9.851379e-1, 5.340271e-1, 9.013672e-1, 6.349182e-1,
		7.226257e-1, 1.948547e-1, 7.628174e-1, 9.873657e-1,
		8.112183e-1, 2.715454e-1, 9.734192e-1, 1.443787e-1,
		4.640198e-1, 3.249207e-1, 3.790894e-1, 8.276367e-2,
		5.954590e-1, 2.864380e-1, 9.806824e-1, 7.929077e-1,
		6.292114e-1, 4.887085e-1, 2.905273e-1, 1.301880e-1,
		3.140869e-1, 5.482483e-1, 4.210815e-1, 1.182861e-1
	}, {
		0.000000e+0, 3.155518e-2, 8.581543e-2, 1.364746e-1,
		1.858826e-1, 2.368469e-1, 2.888184e-1, 3.432617e-1,
		4.012451e-1, 4.623108e-1, 5.271301e-1, 5.954895e-1,
		6.681213e-1, 7.448425e-1, 8.245239e-1, 9.097290e-1
	}, {
		0.000000e+0, 4.418945e-2, 1.303711e-1, 2.273560e-1,
		3.395996e-1, 4.735718e-1, 6.267090e-1, 8.003845e-1
	}, {
		0.000000e+0, 2.804565e-2, 9.683228e-2, 1.849976e-1,
		3.005981e-1, 4.470520e-1, 6.168518e-1, 8.007813e-1
	}
};

static const double BexMode0Bands4[5][16] =
{
	{
		0.000000e+0, 2.708740e-1, 3.479614e-1, 3.578186e-1,
		5.083618e-1, 5.299072e-1, 5.819092e-1, 6.381836e-1,
		7.276917e-1, 7.595520e-1, 7.878723e-1, 9.707336e-1,
		9.713135e-1, 9.736023e-1, 9.759827e-1, 9.832458e-1
	}, {
		0.000000e+0, 2.330627e-1, 5.891418e-1, 7.170410e-1,
		2.036438e-1, 1.613464e-1, 6.668701e-1, 9.481201e-1,
		9.769897e-1, 5.111694e-1, 3.522644e-1, 8.209534e-1,
		2.933960e-1, 9.757690e-1, 5.289917e-1, 4.372253e-1
	}, {
		0.000000e+0, 4.360962e-2, 1.056519e-1, 1.590576e-1,
		2.078857e-1, 2.572937e-1, 3.082581e-1, 3.616028e-1,
		4.191589e-1, 4.792175e-1, 5.438538e-1, 6.125183e-1,
		6.841125e-1, 7.589417e-1, 8.365173e-1, 9.148254e-1
	}, {
		0.000000e+0, 4.074097e-2, 1.164551e-1, 2.077026e-1,
		3.184509e-1, 4.532166e-1, 6.124268e-1, 7.932129e-1
	}, {
		0.000000e+0, 8.880615e-3, 2.932739e-2, 5.593872e-2,
		8.825684e-2, 1.259155e-1, 1.721497e-1, 2.270813e-1,
		2.901611e-1, 3.579712e-1, 4.334106e-1, 5.147095e-1,
		6.023254e-1, 6.956177e-1, 7.952881e-1, 8.977356e-1
	}
};

static const double BexMode0Bands5[3][32] =
{
	{
