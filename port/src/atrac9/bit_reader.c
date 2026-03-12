#include "bit_reader.h"
#include "utility.h"

static int PeekIntFallback(BitReaderCxt* br, int bitCount);

void InitBitReaderCxt(BitReaderCxt* br, const void * buffer)
{
	br->Buffer = buffer;
	br->Position = 0;
}

int ReadInt(BitReaderCxt* br, const int bits)
{
	const int value = PeekInt(br, bits);
	br->Position += bits;
	return value;
}

int ReadSignedInt(BitReaderCxt* br, const int bits)
{
	const int value = PeekInt(br, bits);
	br->Position += bits;
	return SignExtend32(value, bits);
}

int ReadOffsetBinary(BitReaderCxt* br, const int bits)
{
	const int offset = 1 << (bits - 1);
	const int value = PeekInt(br, bits) - offset;
	br->Position += bits;
	return value;
}

int PeekInt(BitReaderCxt* br, const int bits)
{
	const int byteIndex = br->Position / 8;
	const int bitIndex = br->Position % 8;
	const unsigned char* buffer = br->Buffer;

	if (bits <= 9)
	{
		int value = buffer[byteIndex] << 8 | buffer[byteIndex + 1];
		value &= 0xFFFF >> bitIndex;
		value >>= 16 - bits - bitIndex;
		return value;
	}

	if (bits <= 17)
