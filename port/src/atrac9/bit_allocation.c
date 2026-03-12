#include "bit_allocation.h"
#include "tables.h"
#include "utility.h"
#include <string.h>

static unsigned char GradientCurves[48][48];

At9Status CreateGradient(Block* block)
{
	int valueCount = block->GradientEndValue - block->GradientStartValue;
	int unitCount = block->GradientEndUnit - block->GradientStartUnit;

	for (int i = 0; i < block->GradientEndUnit; i++)
	{
		block->Gradient[i] = block->GradientStartValue;
	}

	for (int i = block->GradientEndUnit; i <= block->QuantizationUnitCount; i++)
	{
		block->Gradient[i] = block->GradientEndValue;
	}
	if (unitCount <= 0) return ERR_SUCCESS;
	if (valueCount == 0) return ERR_SUCCESS;

	const unsigned char* curve = GradientCurves[unitCount - 1];
	if (valueCount <= 0)
	{
		double scale = (-valueCount - 1) / 31.0;
		int baseVal = block->GradientStartValue - 1;
		for (int i = block->GradientStartUnit; i < block->GradientEndUnit; i++)
		{
			block->Gradient[i] = baseVal - (int)(curve[i - block->GradientStartUnit] * scale);
		}
	}
	else
	{
		double scale = (valueCount - 1) / 31.0;
		int baseVal = block->GradientStartValue + 1;
		for (int i = block->GradientStartUnit; i < block->GradientEndUnit; i++)
		{
			block->Gradient[i] = baseVal + (int)(curve[i - block->GradientStartUnit] * scale);
		}
	}

	return ERR_SUCCESS;
}

void CalculateMask(Channel* channel)
