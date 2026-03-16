#include "huffCodes.h"
#include "utility.h"
#include <stdint.h>

int ReadHuffmanValue(const HuffmanCodebook* huff, BitReaderCxt* br, int isSigned)
{
	const int code = PeekInt(br, huff->MaxBitSize);
	const unsigned char value = huff->Lookup[code];
	const int bits = huff->Bits[value];
	br->Position += bits;
	return isSigned ? SignExtend32(value, huff->ValueBits) : value;
}

void DecodeHuffmanValues(int* spectrum, int index, int bandCount, const HuffmanCodebook* huff, const int* values)
{
	const int valueCount = bandCount >> huff->ValueCountPower;
	const int mask = (1 << huff->ValueBits) - 1;

	for (int i = 0; i < valueCount; i++)
	{
		int value = values[i];
		for (int j = 0; j < huff->ValueCount; j++)
		{
			spectrum[index++] = SignExtend32(value & mask, huff->ValueBits);
			value >>= huff->ValueBits;
		}
	}
}

void InitHuffmanCodebook(const HuffmanCodebook* codebook)
{
	const int huffLength = codebook->Length;
	if (huffLength == 0) return;

	unsigned char* dest = codebook->Lookup;

	for (int i = 0; i < huffLength; i++)
	{
		if (codebook->Bits[i] == 0) continue;
		const int unusedBits = codebook->MaxBitSize - codebook->Bits[i];

		const int start = codebook->Codes[i] << unusedBits;
		const int length = 1 << unusedBits;
		const int end = start + length;

		for (int j = start; j < end; j++)
		{
			dest[j] = i;
