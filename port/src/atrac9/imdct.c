#include "imdct.h"
#include "tables.h"

static void Dct4(Mdct* mdct, double* input, double* output);

void RunImdct(Mdct* mdct, double* input, double* output)
{
	const int size = 1 << mdct->Bits;
	const int half = size / 2;
	double dctOut[MAX_FRAME_SAMPLES];
	const double* window = ImdctWindow[mdct->Bits - 6];
	double* previous = mdct->ImdctPrevious;

	Dct4(mdct, input, dctOut);

	for (int i = 0; i < half; i++)
	{
		output[i] = window[i] * dctOut[i + half] + previous[i];
		output[i + half] = window[i + half] * -dctOut[size - 1 - i] - previous[i + half];
		previous[i] = window[size - 1 - i] * -dctOut[half - i - 1];
		previous[i + half] = window[half - i - 1] * dctOut[i];
	}
}

static void Dct4(Mdct* mdct, double* input, double* output)
{
	int MdctBits = mdct->Bits;
	int MdctSize = 1 << MdctBits;
	const int* shuffleTable = ShuffleTables[MdctBits];
	const double* sinTable = SinTables[MdctBits];
	const double* cosTable = CosTables[MdctBits];
	double dctTemp[MAX_FRAME_SAMPLES];

	int size = MdctSize;
	int lastIndex = size - 1;
	int halfSize = size / 2;

	for (int i = 0; i < halfSize; i++)
	{
		int i2 = i * 2;
		double a = input[i2];
		double b = input[lastIndex - i2];
		double sin = sinTable[i];
		double cos = cosTable[i];
		dctTemp[i2] = a * cos + b * sin;
		dctTemp[i2 + 1] = a * sin - b * cos;
	}
	int stageCount = MdctBits - 1;
