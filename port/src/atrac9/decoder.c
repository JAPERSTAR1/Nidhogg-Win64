#include "decoder.h"
#include "band_extension.h"
#include "bit_reader.h"
#include "imdct.h"
#include "quantization.h"
#include "tables.h"
#include "unpack.h"
#include "utility.h"
#include <string.h>

static At9Status DecodeFrame(Frame* frame, BitReaderCxt* br);
static void ImdctBlock(Block* block);
static void ApplyIntensityStereo(Block* block);
static void PcmFloatToShort(Frame* frame, short* pcmOut);

At9Status Decode(Atrac9Handle* handle, const unsigned char* audio, unsigned char* pcm, int* bytesUsed)
{
	BitReaderCxt br;
	InitBitReaderCxt(&br, audio);
	ERROR_CHECK(DecodeFrame(&handle->Frame, &br));

	PcmFloatToShort(&handle->Frame, (short*)pcm);

	*bytesUsed = br.Position / 8;
	return ERR_SUCCESS;
}

static At9Status DecodeFrame(Frame* frame, BitReaderCxt* br)
{
	ERROR_CHECK(UnpackFrame(frame, br));

	for (int i = 0; i < frame->Config->ChannelConfig.BlockCount; i++)
	{
		Block* block = &frame->Blocks[i];

		DequantizeSpectra(block);
		ApplyIntensityStereo(block);
		ScaleSpectrumBlock(block);
		ApplyBandExtension(block);
		ImdctBlock(block);
	}

	return ERR_SUCCESS;
}

void PcmFloatToShort(Frame* frame, short* pcmOut)
{
	const int channelCount = frame->Config->ChannelCount;
