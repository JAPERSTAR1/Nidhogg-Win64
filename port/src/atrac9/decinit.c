#include "decinit.h"
#include "bit_allocation.h"
#include "bit_reader.h"
#include "error_codes.h"
#include "huffCodes.h"
#include "structures.h"
#include "tables.h"
#include "utility.h"
#include <math.h>
#include <string.h>

static At9Status InitConfigData(ConfigData* config, unsigned char * configData);
static At9Status ReadConfigData(ConfigData* config);
static At9Status InitFrame(Atrac9Handle* handle);
static At9Status InitBlock(Block* block, Frame* parentFrame, int blockIndex);
static At9Status InitChannel(Channel* channel, Block* parentBlock, int channelIndex);
static void InitHuffmanCodebooks();
static void InitHuffmanSet(const HuffmanCodebook* codebooks, int count);
static void GenerateTrigTables(int sizeBits);
static void GenerateShuffleTable(int sizeBits);
static void InitMdctTables(int frameSizePower);
static void GenerateMdctWindow(int frameSizePower);
static void GenerateImdctWindow(int frameSizePower);

static int BlockTypeToChannelCount(BlockType blockType);

At9Status InitDecoder(Atrac9Handle* handle, unsigned char* configData, int wlength)
{
	ERROR_CHECK(InitConfigData(&handle->Config, configData));
	ERROR_CHECK(InitFrame(handle));
	InitMdctTables(handle->Config.FrameSamplesPower);
	InitHuffmanCodebooks();
	GenerateGradientCurves();
	handle->Wlength = wlength;
	handle->Initialized = 1;
	return ERR_SUCCESS;
}

static At9Status InitConfigData(ConfigData* config, unsigned char* configData)
{
	memcpy(config->ConfigData, configData, CONFIG_DATA_SIZE);
	ERROR_CHECK(ReadConfigData(config));

	config->FramesPerSuperframe = 1 << config->SuperframeIndex;
	config->SuperframeBytes = config->FrameBytes << config->SuperframeIndex;

	config->ChannelConfig = ChannelConfigs[config->ChannelConfigIndex];
	config->ChannelCount = config->ChannelConfig.ChannelCount;
