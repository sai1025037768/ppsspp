// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include "Common/Serialize/Serializer.h"
#include "Common/Serialize/SerializeFuncs.h"
#include "Common/Log.h"
#include "Core/Reporting.h"
#include "Core/MemMapHelpers.h"
#include "Core/System.h"
#include "Core/HLE/HLE.h"
#include "Core/HLE/FunctionWrappers.h"
#include "Core/HLE/sceAtrac.h"
#include "Core/HLE/AtracCtx.h"
#include "Core/HW/Atrac3Standalone.h"
#include "Core/HLE/sceKernelMemory.h"

const size_t overAllocBytes = 16384;

const int RIFF_CHUNK_MAGIC = 0x46464952;
const int RIFF_WAVE_MAGIC = 0x45564157;
const int FMT_CHUNK_MAGIC = 0x20746D66;
const int DATA_CHUNK_MAGIC = 0x61746164;
const int SMPL_CHUNK_MAGIC = 0x6C706D73;
const int FACT_CHUNK_MAGIC = 0x74636166;

void Atrac::DoState(PointerWrap &p) {
	auto s = p.Section("Atrac", 1, 9);
	if (!s)
		return;

	Do(p, channels_);
	Do(p, outputChannels_);
	if (s >= 5) {
		Do(p, jointStereo_);
	}

	Do(p, atracID_);
	Do(p, first_);
	Do(p, bufferMaxSize_);
	Do(p, codecType_);

	Do(p, currentSample_);
	Do(p, endSample_);
	Do(p, firstSampleOffset_);
	if (s >= 3) {
		Do(p, dataOff_);
	} else {
		dataOff_ = firstSampleOffset_;
	}

	u32 hasDataBuf = dataBuf_ != nullptr;
	Do(p, hasDataBuf);
	if (hasDataBuf) {
		if (p.mode == p.MODE_READ) {
			if (dataBuf_)
				delete[] dataBuf_;
			dataBuf_ = new u8[first_.filesize + overAllocBytes];
			memset(dataBuf_, 0, first_.filesize + overAllocBytes);
		}
		DoArray(p, dataBuf_, first_.filesize);
	}
	Do(p, second_);

	Do(p, decodePos_);
	if (s < 9) {
		u32 oldDecodeEnd = 0;
		Do(p, oldDecodeEnd);
	}
	if (s >= 4) {
		Do(p, bufferPos_);
	} else {
		bufferPos_ = decodePos_;
	}

	Do(p, bitrate_);
	Do(p, bytesPerFrame_);

	Do(p, loopinfo_);
	if (s < 9) {
		int oldLoopInfoNum = 42;
		Do(p, oldLoopInfoNum);
	}

	Do(p, loopStartSample_);
	Do(p, loopEndSample_);
	Do(p, loopNum_);

	Do(p, context_);
	if (s >= 6) {
		Do(p, bufferState_);
	} else {
		if (dataBuf_ == nullptr) {
			bufferState_ = ATRAC_STATUS_NO_DATA;
		} else {
			UpdateBufferState();
		}
	}

	if (s >= 7) {
		Do(p, ignoreDataBuf_);
	} else {
		ignoreDataBuf_ = false;
	}

	if (s >= 9) {
		Do(p, bufferValidBytes_);
		Do(p, bufferHeaderSize_);
	} else {
		bufferHeaderSize_ = dataOff_;
		bufferValidBytes_ = std::min(first_.size - dataOff_, StreamBufferEnd() - dataOff_);
		if ((bufferState_ & ATRAC_STATUS_STREAMED_MASK) == ATRAC_STATUS_STREAMED_MASK) {
			bufferPos_ = dataOff_;
		}
	}

	if (s < 8 && bufferState_ == ATRAC_STATUS_STREAMED_LOOP_WITH_TRAILER) {
		// We didn't actually allow the second buffer to be set this far back.
		// Pretend it's a regular loop, we'll just try our best.
		bufferState_ = ATRAC_STATUS_STREAMED_LOOP_FROM_END;
	}

	// Make sure to do this late; it depends on things like bytesPerFrame_.
	if (p.mode == p.MODE_READ && bufferState_ != ATRAC_STATUS_NO_DATA) {
		CreateDecoder();
	}

	if (s >= 2 && s < 9) {
		bool oldResetBuffer = false;
		Do(p, oldResetBuffer);
	}
}

void Atrac::ResetData() {
	delete decoder_;
	decoder_ = nullptr;

	if (dataBuf_)
		delete[] dataBuf_;
	dataBuf_ = 0;
	ignoreDataBuf_ = false;
	bufferState_ = ATRAC_STATUS_NO_DATA;

	if (context_.IsValid())
		kernelMemory.Free(context_.ptr);
}

void Atrac::AnalyzeReset() {
	// Reset some values.
	codecType_ = 0;
	currentSample_ = 0;
	endSample_ = -1;
	loopNum_ = 0;
	loopinfo_.clear();
	loopStartSample_ = -1;
	loopEndSample_ = -1;
	decodePos_ = 0;
	bufferPos_ = 0;
	channels_ = 2;
}

void Atrac::UpdateContextFromPSPMem() {
	if (!context_.IsValid()) {
		return;
	}

	// Read in any changes from the game to the context.
	// TODO: Might be better to just always track in RAM.
	bufferState_ = context_->info.state;
	// This value is actually abused by games to store the SAS voice number.
	loopNum_ = context_->info.loopNum;
}

void Atrac::WriteContextToPSPMem() {
	if (!context_.IsValid()) {
		return;
	}
	// context points into PSP memory.
	SceAtracContext *context = context_;
	context->info.buffer = first_.addr;
	context->info.bufferByte = bufferMaxSize_;
	context->info.secondBuffer = second_.addr;
	context->info.secondBufferByte = second_.size;
	context->info.codec = codecType_;
	context->info.loopNum = loopNum_;
	context->info.loopStart = loopStartSample_ > 0 ? loopStartSample_ : 0;
	context->info.loopEnd = loopEndSample_ > 0 ? loopEndSample_ : 0;

	// Note that we read in the state when loading the atrac object, so it's safe
	// to update it back here all the time.  Some games, like Sol Trigger, change it.
	// TODO: Should we just keep this in PSP ram then, or something?
	context->info.state = bufferState_;
	if (firstSampleOffset_ != 0) {
		context->info.samplesPerChan = firstSampleOffset_ + FirstOffsetExtra();
	} else {
		context->info.samplesPerChan = (codecType_ == PSP_MODE_AT_3_PLUS ? ATRAC3PLUS_MAX_SAMPLES : ATRAC3_MAX_SAMPLES);
	}
	context->info.sampleSize = bytesPerFrame_;
	context->info.numChan = channels_;
	context->info.dataOff = dataOff_;
	context->info.endSample = endSample_ + firstSampleOffset_ + FirstOffsetExtra();
	context->info.dataEnd = first_.filesize;
	context->info.curOff = first_.fileoffset;
	context->info.decodePos = DecodePosBySample(currentSample_);
	context->info.streamDataByte = first_.size - dataOff_;

	u8 *buf = (u8 *)context;
	*(u32_le *)(buf + 0xfc) = atracID_;

	NotifyMemInfo(MemBlockFlags::WRITE, context_.ptr, sizeof(SceAtracContext), "AtracContext");
}

int Atrac::Analyze(u32 addr, u32 size) {
	struct RIFFFmtChunk {
		u16_le fmtTag;
		u16_le channels;
		u32_le samplerate;
		u32_le avgBytesPerSec;
		u16_le blockAlign;
	};

	first_.addr = addr;
	first_.size = size;

	AnalyzeReset();

	// 72 is about the size of the minimum required data to even be valid.
	if (first_.size < 72) {
		return hleReportError(ME, ATRAC_ERROR_SIZE_TOO_SMALL, "buffer too small");
	}

	// TODO: Check the range (addr, size) instead.
	if (!Memory::IsValidAddress(first_.addr)) {
		return hleReportWarning(ME, SCE_KERNEL_ERROR_ILLEGAL_ADDRESS, "invalid buffer address");
	}

	// TODO: Validate stuff.
	if (Memory::ReadUnchecked_U32(first_.addr) != RIFF_CHUNK_MAGIC) {
		return hleReportError(ME, ATRAC_ERROR_UNKNOWN_FORMAT, "invalid RIFF header");
	}

	u32 offset = 8;
	firstSampleOffset_ = 0;

	while (Memory::Read_U32(first_.addr + offset) != RIFF_WAVE_MAGIC) {
		// Get the size preceding the magic.
		int chunk = Memory::Read_U32(first_.addr + offset - 4);
		// Round the chunk size up to the nearest 2.
		offset += chunk + (chunk & 1);
		if (offset + 12 > first_.size) {
			return hleReportError(ME, ATRAC_ERROR_SIZE_TOO_SMALL, "too small for WAVE chunk at %d", offset);
		}
		if (Memory::Read_U32(first_.addr + offset) != RIFF_CHUNK_MAGIC) {
			return hleReportError(ME, ATRAC_ERROR_UNKNOWN_FORMAT, "RIFF chunk did not contain WAVE");
		}
		offset += 8;
	}
	offset += 4;

	if (offset != 12) {
		WARN_LOG_REPORT(ME, "RIFF chunk at offset: %d", offset);
	}

	// RIFF size excluding chunk header.
	first_.filesize = Memory::Read_U32(first_.addr + offset - 8) + 8;
	// Even if the RIFF size is too low, it may simply be incorrect.  This works on real firmware.
	u32 maxSize = std::max(first_.filesize, first_.size);

	bool bfoundData = false;
	u32 dataChunkSize = 0;
	int sampleOffsetAdjust = 0;
	while (maxSize >= offset + 8 && !bfoundData) {
		int chunkMagic = Memory::Read_U32(first_.addr + offset);
		u32 chunkSize = Memory::Read_U32(first_.addr + offset + 4);
		// Account for odd sized chunks.
		if (chunkSize & 1) {
			WARN_LOG_REPORT_ONCE(oddchunk, ME, "RIFF chunk had uneven size");
		}
		chunkSize += (chunkSize & 1);
		offset += 8;
		if (chunkSize > maxSize - offset)
			break;
		switch (chunkMagic) {
		case FMT_CHUNK_MAGIC:
		{
			if (codecType_ != 0) {
				return hleReportError(ME, ATRAC_ERROR_UNKNOWN_FORMAT, "multiple fmt definitions");
			}

			auto at3fmt = PSPPointer<const RIFFFmtChunk>::Create(first_.addr + offset);
			if (chunkSize < 32 || (at3fmt->fmtTag == AT3_PLUS_MAGIC && chunkSize < 52)) {
				return hleReportError(ME, ATRAC_ERROR_UNKNOWN_FORMAT, "fmt definition too small (%d)", chunkSize);
			}

			if (at3fmt->fmtTag == AT3_MAGIC)
				codecType_ = PSP_MODE_AT_3;
			else if (at3fmt->fmtTag == AT3_PLUS_MAGIC)
				codecType_ = PSP_MODE_AT_3_PLUS;
			else {
				return hleReportError(ME, ATRAC_ERROR_UNKNOWN_FORMAT, "invalid fmt magic: %04x", at3fmt->fmtTag);
			}
			channels_ = at3fmt->channels;
			if (channels_ != 1 && channels_ != 2) {
				return hleReportError(ME, ATRAC_ERROR_UNKNOWN_FORMAT, "invalid channel count: %d", channels_);
			}
			if (at3fmt->samplerate != 44100) {
				return hleReportError(ME, ATRAC_ERROR_UNKNOWN_FORMAT, "unsupported sample rate: %d", at3fmt->samplerate);
			}
			bitrate_ = at3fmt->avgBytesPerSec * 8;
			bytesPerFrame_ = at3fmt->blockAlign;
			if (bytesPerFrame_ == 0) {
				return hleReportError(ME, ATRAC_ERROR_UNKNOWN_FORMAT, "invalid bytes per frame: %d", bytesPerFrame_);
			}

			// TODO: There are some format specific bytes here which seem to have fixed values?
			// Probably don't need them.

			if (at3fmt->fmtTag == AT3_MAGIC) {
				// This is the offset to the jointStereo_ field.
				jointStereo_ = Memory::Read_U32(first_.addr + offset + 24);
			}
		}
		break;
		case FACT_CHUNK_MAGIC:
		{
			endSample_ = Memory::Read_U32(first_.addr + offset);
			if (chunkSize >= 8) {
				firstSampleOffset_ = Memory::Read_U32(first_.addr + offset + 4);
			}
			if (chunkSize >= 12) {
				u32 largerOffset = Memory::Read_U32(first_.addr + offset + 8);
				sampleOffsetAdjust = firstSampleOffset_ - largerOffset;
			}
		}
		break;
		case SMPL_CHUNK_MAGIC:
		{
			if (chunkSize < 32) {
				return hleReportError(ME, ATRAC_ERROR_UNKNOWN_FORMAT, "smpl chunk too small (%d)", chunkSize);
			}
			int checkNumLoops = Memory::Read_U32(first_.addr + offset + 28);
			if (checkNumLoops != 0 && chunkSize < 36 + 20) {
				return hleReportError(ME, ATRAC_ERROR_UNKNOWN_FORMAT, "smpl chunk too small for loop (%d, %d)", checkNumLoops, chunkSize);
			}
			if (checkNumLoops < 0) {
				return hleReportError(ME, ATRAC_ERROR_UNKNOWN_FORMAT, "bad checkNumLoops (%d)", checkNumLoops);
			}

			loopinfo_.resize(checkNumLoops);
			u32 loopinfoAddr = first_.addr + offset + 36;
			// The PSP only cares about the first loop start and end, it seems.
			// Most likely can skip the rest of this data, but it's not hurting anyone.
			for (int i = 0; i < checkNumLoops && 36 + (u32)i < chunkSize; i++, loopinfoAddr += 24) {
				loopinfo_[i].cuePointID = Memory::Read_U32(loopinfoAddr);
				loopinfo_[i].type = Memory::Read_U32(loopinfoAddr + 4);
				loopinfo_[i].startSample = Memory::Read_U32(loopinfoAddr + 8);
				loopinfo_[i].endSample = Memory::Read_U32(loopinfoAddr + 12);
				loopinfo_[i].fraction = Memory::Read_U32(loopinfoAddr + 16);
				loopinfo_[i].playCount = Memory::Read_U32(loopinfoAddr + 20);

				if (loopinfo_[i].startSample >= loopinfo_[i].endSample) {
					return hleReportError(ME, ATRAC_ERROR_BAD_CODEC_PARAMS, "loop starts after it ends");
				}
			}
		}
		break;
		case DATA_CHUNK_MAGIC:
		{
			bfoundData = true;
			dataOff_ = offset;
			dataChunkSize = chunkSize;
			if (first_.filesize < offset + chunkSize) {
				WARN_LOG_REPORT(ME, "Atrac data chunk extends beyond riff chunk");
				first_.filesize = offset + chunkSize;
			}
		}
		break;
		}
		offset += chunkSize;
	}

	if (codecType_ == 0) {
		return hleReportError(ME, ATRAC_ERROR_UNKNOWN_FORMAT, "could not detect codec");
	}

	if (!bfoundData) {
		return hleReportError(ME, ATRAC_ERROR_SIZE_TOO_SMALL, "no data chunk");
	}

	// set the loopStartSample_ and loopEndSample_ by loopinfo_
	if (loopinfo_.size() > 0) {
		loopStartSample_ = loopinfo_[0].startSample + FirstOffsetExtra() + sampleOffsetAdjust;
		loopEndSample_ = loopinfo_[0].endSample + FirstOffsetExtra() + sampleOffsetAdjust;
	} else {
		loopStartSample_ = -1;
		loopEndSample_ = -1;
	}

	// if there is no correct endsample, try to guess it
	if (endSample_ <= 0 && bytesPerFrame_ != 0) {
		endSample_ = (dataChunkSize / bytesPerFrame_) * SamplesPerFrame();
		endSample_ -= firstSampleOffset_ + FirstOffsetExtra();
	}
	endSample_ -= 1;

	if (loopEndSample_ != -1 && loopEndSample_ > endSample_ + firstSampleOffset_ + (int)FirstOffsetExtra()) {
		return hleReportError(ME, ATRAC_ERROR_BAD_CODEC_PARAMS, "loop after end of data");
	}

	return 0;
}

int Atrac::AnalyzeAA3(u32 addr, u32 size, u32 filesize) {
	first_.addr = addr;
	first_.size = size;
	first_.filesize = filesize;

	AnalyzeReset();

	if (first_.size < 10) {
		return hleReportError(ME, ATRAC_ERROR_AA3_SIZE_TOO_SMALL, "buffer too small");
	}

	// TODO: Make sure this validation is correct, more testing.

	const u8 *buffer = Memory::GetPointer(first_.addr);
	if (buffer[0] != 'e' || buffer[1] != 'a' || buffer[2] != '3') {
		return hleReportError(ME, ATRAC_ERROR_AA3_INVALID_DATA, "invalid ea3 magic bytes");
	}

	// It starts with an id3 header (replaced with ea3.)  This is the size.
	u32 tagSize = buffer[9] | (buffer[8] << 7) | (buffer[7] << 14) | (buffer[6] << 21);
	if (first_.size < tagSize + 36) {
		return hleReportError(ME, ATRAC_ERROR_AA3_SIZE_TOO_SMALL, "truncated before id3 end");
	}

	// EA3 header starts at id3 header (10) + tagSize.
	buffer = Memory::GetPointer(first_.addr + 10 + tagSize);
	if (buffer[0] != 'E' || buffer[1] != 'A' || buffer[2] != '3') {
		return hleReportError(ME, ATRAC_ERROR_AA3_INVALID_DATA, "invalid EA3 magic bytes");
	}

	// Based on FFmpeg's code.
	u32 codecParams = buffer[35] | (buffer[34] << 8) | (buffer[35] << 16);
	const u32 at3SampleRates[8] = { 32000, 44100, 48000, 88200, 96000, 0 };

	switch (buffer[32]) {
	case 0:
		codecType_ = PSP_MODE_AT_3;
		bytesPerFrame_ = (codecParams & 0x03FF) * 8;
		bitrate_ = at3SampleRates[(codecParams >> 13) & 7] * bytesPerFrame_ * 8 / 1024;
		channels_ = 2;
		jointStereo_ = (codecParams >> 17) & 1;
		break;
	case 1:
		codecType_ = PSP_MODE_AT_3_PLUS;
		bytesPerFrame_ = ((codecParams & 0x03FF) * 8) + 8;
		bitrate_ = at3SampleRates[(codecParams >> 13) & 7] * bytesPerFrame_ * 8 / 2048;
		channels_ = (codecParams >> 10) & 7;
		break;
	case 3:
	case 4:
	case 5:
		return hleReportError(ME, ATRAC_ERROR_AA3_INVALID_DATA, "unsupported codec type %d", buffer[32]);
	default:
		return hleReportError(ME, ATRAC_ERROR_AA3_INVALID_DATA, "invalid codec type %d", buffer[32]);
	}

	dataOff_ = 10 + tagSize + 96;
	firstSampleOffset_ = 0;
	if (endSample_ < 0 && bytesPerFrame_ != 0) {
		endSample_ = ((first_.filesize - dataOff_) / bytesPerFrame_) * SamplesPerFrame();
	}
	endSample_ -= 1;

	return 0;
}

void Atrac::CalculateStreamInfo(u32 *outReadOffset) {
	u32 readOffset = first_.fileoffset;
	if (bufferState_ == ATRAC_STATUS_ALL_DATA_LOADED) {
		// Nothing to write.
		readOffset = 0;
		first_.offset = 0;
		first_.writableBytes = 0;
	} else if (bufferState_ == ATRAC_STATUS_HALFWAY_BUFFER) {
		// If we're buffering the entire file, just give the same as readOffset.
		first_.offset = readOffset;
		// In this case, the bytes writable are just the remaining bytes, always.
		first_.writableBytes = first_.filesize - readOffset;
	} else {
		u32 bufferEnd = StreamBufferEnd();
		u32 bufferValidExtended = bufferPos_ + bufferValidBytes_;
		if (bufferValidExtended < bufferEnd) {
			first_.offset = bufferValidExtended;
			first_.writableBytes = bufferEnd - bufferValidExtended;
		} else {
			u32 bufferStartUsed = bufferValidExtended - bufferEnd;
			first_.offset = bufferStartUsed;
			first_.writableBytes = bufferPos_ - bufferStartUsed;
		}

		if (readOffset >= first_.filesize) {
			if (bufferState_ == ATRAC_STATUS_STREAMED_WITHOUT_LOOP) {
				// We don't need anything more, so all 0s.
				readOffset = 0;
				first_.offset = 0;
				first_.writableBytes = 0;
			} else {
				readOffset = FileOffsetBySample(loopStartSample_ - FirstOffsetExtra() - firstSampleOffset_ - SamplesPerFrame() * 2);
			}
		}

		if (readOffset + first_.writableBytes > first_.filesize) {
			// Never ask for past the end of file, even when the space is free.
			first_.writableBytes = first_.filesize - readOffset;
		}

		// If you don't think this should be here, remove it.  It's just a temporary safety check.
		if (first_.offset + first_.writableBytes > bufferMaxSize_) {
			ERROR_LOG_REPORT(ME, "Somehow calculated too many writable bytes: %d + %d > %d", first_.offset, first_.writableBytes, bufferMaxSize_);
			first_.offset = 0;
			first_.writableBytes = bufferMaxSize_;
		}
	}

	if (outReadOffset) {
		*outReadOffset = readOffset;
	}
}

void Atrac::CreateDecoder() {
	if (decoder_) {
		delete decoder_;
	}

	// First, init the standalone decoder. Only used for low-level-decode initially, but simple.
	if (codecType_ == PSP_MODE_AT_3) {
		// We don't pull this from the RIFF so that we can support OMA also.
		uint8_t extraData[14]{};
		// The only thing that changes are the jointStereo_ values.
		extraData[0] = 1;
		extraData[3] = channels_ << 3;
		extraData[6] = jointStereo_;
		extraData[8] = jointStereo_;
		extraData[10] = 1;
		decoder_ = CreateAtrac3Audio(channels_, bytesPerFrame_, extraData, sizeof(extraData));
	} else {
		decoder_ = CreateAtrac3PlusAudio(channels_, bytesPerFrame_);
	}
	// reinit decodePos, because ffmpeg had changed it.
	decodePos_ = 0;
}

void Atrac::GetResetBufferInfo(AtracResetBufferInfo *bufferInfo, int sample) {
	if (bufferState_ == ATRAC_STATUS_ALL_DATA_LOADED) {
		bufferInfo->first.writePosPtr = first_.addr;
		// Everything is loaded, so nothing needs to be read.
		bufferInfo->first.writableBytes = 0;
		bufferInfo->first.minWriteBytes = 0;
		bufferInfo->first.filePos = 0;
	} else if (bufferState_ == ATRAC_STATUS_HALFWAY_BUFFER) {
		// Here the message is: you need to read at least this many bytes to get to that position.
		// This is because we're filling the buffer start to finish, not streaming.
		bufferInfo->first.writePosPtr = first_.addr + first_.size;
		bufferInfo->first.writableBytes = first_.filesize - first_.size;
		int minWriteBytes = FileOffsetBySample(sample) - first_.size;
		if (minWriteBytes > 0) {
			bufferInfo->first.minWriteBytes = minWriteBytes;
		} else {
			bufferInfo->first.minWriteBytes = 0;
		}
		bufferInfo->first.filePos = first_.size;
	} else {
		// This is without the sample offset.  The file offset also includes the previous batch of samples?
		int sampleFileOffset = FileOffsetBySample(sample - firstSampleOffset_ - SamplesPerFrame());

		// Update the writable bytes.  When streaming, this is just the number of bytes until the end.
		const u32 bufSizeAligned = (bufferMaxSize_ / bytesPerFrame_) * bytesPerFrame_;
		const int needsMoreFrames = FirstOffsetExtra();

		bufferInfo->first.writePosPtr = first_.addr;
		bufferInfo->first.writableBytes = std::min(first_.filesize - sampleFileOffset, bufSizeAligned);
		if (((sample + firstSampleOffset_) % (int)SamplesPerFrame()) >= (int)SamplesPerFrame() - needsMoreFrames) {
			// Not clear why, but it seems it wants a bit extra in case the sample is late?
			bufferInfo->first.minWriteBytes = bytesPerFrame_ * 3;
		} else {
			bufferInfo->first.minWriteBytes = bytesPerFrame_ * 2;
		}
		if ((u32)sample < (u32)firstSampleOffset_ && sampleFileOffset != dataOff_) {
			sampleFileOffset -= bytesPerFrame_;
		}
		bufferInfo->first.filePos = sampleFileOffset;

		if (second_.size != 0) {
			// TODO: We have a second buffer.  Within it, minWriteBytes should be zero.
			// The filePos should be after the end of the second buffer (or zero.)
			// We actually need to ensure we READ from the second buffer before implementing that.
		}
	}

	// It seems like this is always the same as the first buffer's pos, weirdly.
	bufferInfo->second.writePosPtr = first_.addr;
	// Reset never needs a second buffer write, since the loop is in a fixed place.
	bufferInfo->second.writableBytes = 0;
	bufferInfo->second.minWriteBytes = 0;
	bufferInfo->second.filePos = 0;
}

int Atrac::SetData(u32 buffer, u32 readSize, u32 bufferSize, int successCode) {
	first_.addr = buffer;
	first_.size = readSize;

	if (first_.size > first_.filesize)
		first_.size = first_.filesize;
	first_.fileoffset = first_.size;

	// got the size of temp buf, and calculate offset
	bufferMaxSize_ = bufferSize;
	first_.offset = first_.size;

	// some games may reuse an atracID for playing sound
	ResetData();
	UpdateBufferState();

	if (codecType_ != PSP_MODE_AT_3 && codecType_ != PSP_MODE_AT_3_PLUS) {
		// Shouldn't have gotten here, Analyze() checks this.
		bufferState_ = ATRAC_STATUS_NO_DATA;
		return hleReportError(ME, ATRAC_ERROR_UNKNOWN_FORMAT, "unexpected codec type in set data");
	}

	if (bufferState_ == ATRAC_STATUS_ALL_DATA_LOADED || bufferState_ == ATRAC_STATUS_HALFWAY_BUFFER) {
		// This says, don't use the dataBuf_ array, use the PSP RAM.
		// This way, games can load data async into the buffer, and it still works.
		// TODO: Support this always, even for streaming.
		ignoreDataBuf_ = true;
	}
	if (bufferState_ == ATRAC_STATUS_STREAMED_WITHOUT_LOOP || bufferState_ == ATRAC_STATUS_STREAMED_LOOP_FROM_END || bufferState_ == ATRAC_STATUS_STREAMED_LOOP_WITH_TRAILER) {
		bufferHeaderSize_ = dataOff_;
		bufferPos_ = dataOff_ + bytesPerFrame_;
		bufferValidBytes_ = first_.size - bufferPos_;
	}

	const char *codecName = codecType_ == PSP_MODE_AT_3 ? "atrac3" : "atrac3+";
	const char *channelName = channels_ == 1 ? "mono" : "stereo";

	// Over-allocate databuf to prevent going off the end if the bitstream is bad or if there are
	// bugs in the decoder. This happens, see issue #15788. Arbitrary, but let's make it a whole page on the popular
	// architecture that has the largest pages (M1).
	dataBuf_ = new u8[first_.filesize + overAllocBytes];
	memset(dataBuf_, 0, first_.filesize + overAllocBytes);
	if (!ignoreDataBuf_) {
		u32 copybytes = std::min(bufferSize, first_.filesize);
		Memory::Memcpy(dataBuf_, buffer, copybytes, "AtracSetData");
	}
	CreateDecoder();
	return hleLogSuccessInfoI(ME, successCode, "%s %s audio", codecName, channelName);
}

u32 Atrac::SetSecondBuffer(u32 secondBuffer, u32 secondBufferSize) {
	u32 secondFileOffset = FileOffsetBySample(loopEndSample_ - firstSampleOffset_);
	u32 desiredSize = first_.filesize - secondFileOffset;

	// 3 seems to be the number of frames required to handle a loop.
	if (secondBufferSize < desiredSize && secondBufferSize < (u32)BytesPerFrame() * 3) {
		return hleReportError(ME, ATRAC_ERROR_SIZE_TOO_SMALL, "too small");
	}
	if (BufferState() != ATRAC_STATUS_STREAMED_LOOP_WITH_TRAILER) {
		return hleReportError(ME, ATRAC_ERROR_SECOND_BUFFER_NOT_NEEDED, "not needed");
	}

	second_.addr = secondBuffer;
	second_.size = secondBufferSize;
	second_.fileoffset = secondFileOffset;
	return hleLogSuccessI(ME, 0);
}

void Atrac::UpdateBitrate() {
	bitrate_ = (bytesPerFrame_ * 352800) / 1000;
	if (codecType_ == PSP_MODE_AT_3_PLUS)
		bitrate_ = ((bitrate_ >> 11) + 8) & 0xFFFFFFF0;
	else
		bitrate_ = (bitrate_ + 511) >> 10;
}

int Atrac::AddStreamData(u32 bytesToAdd) {
	u32 readOffset;
	CalculateStreamInfo(&readOffset);
	if (bytesToAdd > first_.writableBytes)
		return hleLogWarning(ME, ATRAC_ERROR_ADD_DATA_IS_TOO_BIG, "too many bytes");

	if (bytesToAdd > 0) {
		first_.fileoffset = readOffset;
		int addbytes = std::min(bytesToAdd, first_.filesize - first_.fileoffset);
		if (!ignoreDataBuf_) {
			Memory::Memcpy(dataBuf_ + first_.fileoffset, first_.addr + first_.offset, addbytes, "AtracAddStreamData");
		}
		first_.fileoffset += addbytes;
	}
	first_.size += bytesToAdd;
	if (first_.size >= first_.filesize) {
		first_.size = first_.filesize;
		if (bufferState_ == ATRAC_STATUS_HALFWAY_BUFFER)
			bufferState_ = ATRAC_STATUS_ALL_DATA_LOADED;
		WriteContextToPSPMem();
	}

	first_.offset += bytesToAdd;
	bufferValidBytes_ += bytesToAdd;

	if (PSP_CoreParameter().compat.flags().AtracLoopHack && bufferState_ == ATRAC_STATUS_STREAMED_LOOP_FROM_END && RemainingFrames() > 2) {
		loopNum_++;
		SeekToSample(loopStartSample_ - FirstOffsetExtra() - firstSampleOffset_);
	}

	return 0;
}

u32 Atrac::AddStreamDataSas(u32 bufPtr, u32 bytesToAdd) {
	int addbytes = std::min(bytesToAdd, first_.filesize - first_.fileoffset - FirstOffsetExtra());
	Memory::Memcpy(dataBuf_ + first_.fileoffset + FirstOffsetExtra(), bufPtr, addbytes, "AtracAddStreamData");
	first_.size += bytesToAdd;
	if (first_.size >= first_.filesize) {
		first_.size = first_.filesize;
		if (bufferState_ == ATRAC_STATUS_HALFWAY_BUFFER)
			bufferState_ = ATRAC_STATUS_ALL_DATA_LOADED;
	}
	first_.fileoffset += addbytes;
	// refresh context_
	WriteContextToPSPMem();
	return 0;
}

u32 Atrac::GetNextSamples() {
	// It seems like the PSP aligns the sample position to 0x800...?
	u32 skipSamples = firstSampleOffset_ + FirstOffsetExtra();
	u32 firstSamples = (SamplesPerFrame() - skipSamples) % SamplesPerFrame();
	u32 numSamples = endSample_ + 1 - currentSample_;
	if (currentSample_ == 0 && firstSamples != 0) {
		numSamples = firstSamples;
	}
	u32 unalignedSamples = (skipSamples + currentSample_) % SamplesPerFrame();
	if (unalignedSamples != 0) {
		// We're off alignment, possibly due to a loop.  Force it back on.
		numSamples = SamplesPerFrame() - unalignedSamples;
	}
	if (numSamples > SamplesPerFrame())
		numSamples = SamplesPerFrame();
	if (bufferState_ == ATRAC_STATUS_STREAMED_LOOP_FROM_END && (int)numSamples + currentSample_ > endSample_) {
		bufferState_ = ATRAC_STATUS_ALL_DATA_LOADED;
	}
	return numSamples;
}

u8 *Atrac::BufferStart() {
	return ignoreDataBuf_ ? Memory::GetPointerWrite(first_.addr) : dataBuf_;
}

void Atrac::ForceSeekToSample(int sample) {
	if (decoder_) {
		decoder_->FlushBuffers();
	}
	currentSample_ = sample;
}

void Atrac::SeekToSample(int sample) {
	// It seems like the PSP aligns the sample position to 0x800...?
	const u32 offsetSamples = firstSampleOffset_ + FirstOffsetExtra();
	const u32 unalignedSamples = (offsetSamples + sample) % SamplesPerFrame();
	int seekFrame = sample + offsetSamples - unalignedSamples;

	if ((sample != currentSample_ || sample == 0) && decoder_ != nullptr) {
		// Prefill the decode buffer with packets before the first sample offset.
		decoder_->FlushBuffers();

		int adjust = 0;
		if (sample == 0) {
			int offsetSamples = firstSampleOffset_ + FirstOffsetExtra();
			adjust = -(int)(offsetSamples % SamplesPerFrame());
		}
		const u32 off = FileOffsetBySample(sample + adjust);
		const u32 backfill = bytesPerFrame_ * 2;
		const u32 start = off - dataOff_ < backfill ? dataOff_ : off - backfill;

		for (u32 pos = start; pos < off; pos += bytesPerFrame_) {
			decoder_->Decode(BufferStart() + pos, bytesPerFrame_, nullptr, nullptr, nullptr);
		}
	}

	currentSample_ = sample;
}

int Atrac::RemainingFrames() const {
	if (bufferState_ == ATRAC_STATUS_ALL_DATA_LOADED) {
		// Meaning, infinite I guess?  We've got it all.
		return PSP_ATRAC_ALLDATA_IS_ON_MEMORY;
	}

	u32 currentFileOffset = FileOffsetBySample(currentSample_ - SamplesPerFrame() + FirstOffsetExtra());
	if (first_.fileoffset >= first_.filesize) {
		if (bufferState_ == ATRAC_STATUS_STREAMED_WITHOUT_LOOP) {
			return PSP_ATRAC_NONLOOP_STREAM_DATA_IS_ON_MEMORY;
		}
		int loopEndAdjusted = loopEndSample_ - FirstOffsetExtra() - firstSampleOffset_;
		if (bufferState_ == ATRAC_STATUS_STREAMED_LOOP_WITH_TRAILER && currentSample_ > loopEndAdjusted) {
			// No longer looping in this case, outside the loop.
			return PSP_ATRAC_NONLOOP_STREAM_DATA_IS_ON_MEMORY;
		}
		if ((bufferState_ & ATRAC_STATUS_STREAMED_MASK) == ATRAC_STATUS_STREAMED_MASK && loopNum_ == 0) {
			return PSP_ATRAC_LOOP_STREAM_DATA_IS_ON_MEMORY;
		}
	}

	if ((bufferState_ & ATRAC_STATUS_STREAMED_MASK) == ATRAC_STATUS_STREAMED_MASK) {
		// Since we're streaming, the remaining frames are what's valid in the buffer.
		return bufferValidBytes_ / bytesPerFrame_;
	}

	// Since the first frame is shorter by this offset, add to round up at this offset.
	const int remainingBytes = first_.fileoffset - currentFileOffset;
	if (remainingBytes < 0) {
		// Just in case.  Shouldn't happen, but once did by mistake.
		return 0;
	}
	return remainingBytes / bytesPerFrame_;
}

void Atrac::ConsumeFrame() {
	bufferPos_ += bytesPerFrame_;
	if ((bufferState_ & ATRAC_STATUS_STREAMED_MASK) == ATRAC_STATUS_STREAMED_MASK) {
		if (bufferValidBytes_ > bytesPerFrame_) {
			bufferValidBytes_ -= bytesPerFrame_;
		} else {
			bufferValidBytes_ = 0;
		}
	}
	if (bufferPos_ >= StreamBufferEnd()) {
		// Wrap around... theoretically, this should only happen at exactly StreamBufferEnd.
		bufferPos_ -= StreamBufferEnd();
		bufferHeaderSize_ = 0;
	}
}

u32 Atrac::DecodeData(u8 *outbuf, u32 outbufPtr, u32 *SamplesNum, u32 *finish, int *remains) {
	int loopNum = loopNum_;
	if (bufferState_ == ATRAC_STATUS_FOR_SCESAS) {
		// TODO: Might need more testing.
		loopNum = 0;
	}

	// We already passed the end - return an error (many games check for this.)
	if (currentSample_ >= endSample_ && loopNum == 0) {
		*SamplesNum = 0;
		*finish = 1;
		// refresh context_
		WriteContextToPSPMem();
		return ATRAC_ERROR_ALL_DATA_DECODED;
	}

	// TODO: This isn't at all right, but at least it makes the music "last" some time.
	u32 numSamples = 0;

	// It seems like the PSP aligns the sample position to 0x800...?
	int offsetSamples = firstSampleOffset_ + FirstOffsetExtra();
	int skipSamples = 0;
	u32 maxSamples = endSample_ + 1 - currentSample_;
	u32 unalignedSamples = (offsetSamples + currentSample_) % SamplesPerFrame();
	if (unalignedSamples != 0) {
		// We're off alignment, possibly due to a loop.  Force it back on.
		maxSamples = SamplesPerFrame() - unalignedSamples;
		skipSamples = unalignedSamples;
	}

	if (skipSamples != 0 && bufferHeaderSize_ == 0) {
		// Skip the initial frame used to load state for the looped frame.
		// TODO: We will want to actually read this in.
		ConsumeFrame();
	}

	// TODO: We don't support any other codec type, check seems unnecessary?
	if ((codecType_ == PSP_MODE_AT_3 || codecType_ == PSP_MODE_AT_3_PLUS)) {
		SeekToSample(currentSample_);

		bool gotFrame = false;
		u32 off = FileOffsetBySample(currentSample_ - skipSamples);
		if (off < first_.size) {
			uint8_t *indata = BufferStart() + off;
			int bytesConsumed = 0;
			int outBytes = 0;
			if (!decoder_->Decode(indata, bytesPerFrame_, &bytesConsumed,
				outbuf, &outBytes)) {
				// Decode failed.
				*SamplesNum = 0;
				*finish = 1;
				return ATRAC_ERROR_ALL_DATA_DECODED;
			}
			gotFrame = true;

			numSamples = outBytes / 4;
			uint32_t packetAddr = CurBufferAddress(-skipSamples);
			// got a frame
			int skipped = std::min((u32)skipSamples, numSamples);
			skipSamples -= skipped;
			numSamples = numSamples - skipped;
			// If we're at the end, clamp to samples we want.  It always returns a full chunk.
			numSamples = std::min(maxSamples, numSamples);

			if (packetAddr != 0 && MemBlockInfoDetailed()) {
				char tagData[128];
				size_t tagSize = FormatMemWriteTagAt(tagData, sizeof(tagData), "AtracDecode/", packetAddr, bytesPerFrame_);
				NotifyMemInfo(MemBlockFlags::READ, packetAddr, bytesPerFrame_, tagData, tagSize);
				NotifyMemInfo(MemBlockFlags::WRITE, outbufPtr, outBytes, tagData, tagSize);
			} else {
				NotifyMemInfo(MemBlockFlags::WRITE, outbufPtr, outBytes, "AtracDecode");
			}
			// We only want one frame per call, let's continue the next time.
		}

		if (!gotFrame && currentSample_ < endSample_) {
			// Never got a frame.  We may have dropped a GHA frame or otherwise have a bug.
			// For now, let's try to provide an extra "frame" if possible so games don't infinite loop.
			if (FileOffsetBySample(currentSample_) < first_.filesize) {
				numSamples = std::min(maxSamples, SamplesPerFrame());
				u32 outBytes = numSamples * outputChannels_ * sizeof(s16);
				if (outbuf != nullptr) {
					memset(outbuf, 0, outBytes);
					NotifyMemInfo(MemBlockFlags::WRITE, outbufPtr, outBytes, "AtracDecode");
				}
			}
		}
	}

	*SamplesNum = numSamples;
	// update current sample and decodePos
	currentSample_ += numSamples;
	decodePos_ = DecodePosBySample(currentSample_);

	ConsumeFrame();

	int finishFlag = 0;
	// TODO: Verify.
	bool hitEnd = currentSample_ >= endSample_ || (numSamples == 0 && first_.size >= first_.filesize);
	int loopEndAdjusted = loopEndSample_ - FirstOffsetExtra() - firstSampleOffset_;
	if ((hitEnd || currentSample_ > loopEndAdjusted) && loopNum != 0) {
		SeekToSample(loopStartSample_ - FirstOffsetExtra() - firstSampleOffset_);
		if (bufferState_ != ATRAC_STATUS_FOR_SCESAS) {
			if (loopNum_ > 0)
				loopNum_--;
		}
		if ((bufferState_ & ATRAC_STATUS_STREAMED_MASK) == ATRAC_STATUS_STREAMED_MASK) {
			// Whatever bytes we have left were added from the loop.
			u32 loopOffset = FileOffsetBySample(loopStartSample_ - FirstOffsetExtra() - firstSampleOffset_ - SamplesPerFrame() * 2);
			// TODO: Hmm, need to manage the buffer better.  But don't move fileoffset if we already have valid data.
			if (loopOffset > first_.fileoffset || loopOffset + bufferValidBytes_ < first_.fileoffset) {
				// Skip the initial frame at the start.
				first_.fileoffset = FileOffsetBySample(loopStartSample_ - FirstOffsetExtra() - firstSampleOffset_ - SamplesPerFrame() * 2);
			}
		}
	} else if (hitEnd) {
		finishFlag = 1;

		// Still move forward, so we know that we've read everything.
		// This seems to be reflected in the context as well.
		currentSample_ += SamplesPerFrame() - numSamples;
	}

	*finish = finishFlag;
	*remains = RemainingFrames();
	// refresh context_
	WriteContextToPSPMem();
	return 0;
}

void Atrac::SetLoopNum(int loopNum) {
	// Spammed in MHU
	loopNum_ = loopNum;
	if (loopNum != 0 && loopinfo_.size() == 0) {
		// Just loop the whole audio
		loopStartSample_ = firstSampleOffset_ + FirstOffsetExtra();
		loopEndSample_ = endSample_ + firstSampleOffset_ + FirstOffsetExtra();
	}
	WriteContextToPSPMem();
}

u32 Atrac::ResetPlayPosition(int sample, int bytesWrittenFirstBuf, int bytesWrittenSecondBuf) {
	// Reuse the same calculation as before.
	AtracResetBufferInfo bufferInfo;
	GetResetBufferInfo(&bufferInfo, sample);

	if ((u32)bytesWrittenFirstBuf < bufferInfo.first.minWriteBytes || (u32)bytesWrittenFirstBuf > bufferInfo.first.writableBytes) {
		return hleLogError(ME, ATRAC_ERROR_BAD_FIRST_RESET_SIZE, "first byte count not in valid range");
	}
	if ((u32)bytesWrittenSecondBuf < bufferInfo.second.minWriteBytes || (u32)bytesWrittenSecondBuf > bufferInfo.second.writableBytes) {
		return hleLogError(ME, ATRAC_ERROR_BAD_SECOND_RESET_SIZE, "second byte count not in valid range");
	}

	if (bufferState_ == ATRAC_STATUS_ALL_DATA_LOADED) {
		// Always adds zero bytes.
	} else if (bufferState_ == ATRAC_STATUS_HALFWAY_BUFFER) {
		// Okay, it's a valid number of bytes.  Let's set them up.
		if (bytesWrittenFirstBuf != 0) {
			if (!ignoreDataBuf_) {
				Memory::Memcpy(dataBuf_ + first_.size, first_.addr + first_.size, bytesWrittenFirstBuf, "AtracResetPlayPosition");
			}
			first_.fileoffset += bytesWrittenFirstBuf;
			first_.size += bytesWrittenFirstBuf;
			first_.offset += bytesWrittenFirstBuf;
		}

		// Did we transition to a full buffer?
		if (first_.size >= first_.filesize) {
			first_.size = first_.filesize;
			bufferState_ = ATRAC_STATUS_ALL_DATA_LOADED;
		}
	} else {
		if (bufferInfo.first.filePos > first_.filesize) {
			return hleDelayResult(hleLogError(ME, ATRAC_ERROR_API_FAIL, "invalid file position"), "reset play pos", 200);
		}

		// Move the offset to the specified position.
		first_.fileoffset = bufferInfo.first.filePos;

		if (bytesWrittenFirstBuf != 0) {
			if (!ignoreDataBuf_) {
				Memory::Memcpy(dataBuf_ + first_.fileoffset, first_.addr, bytesWrittenFirstBuf, "AtracResetPlayPosition");
			}
			first_.fileoffset += bytesWrittenFirstBuf;
		}
		first_.size = first_.fileoffset;
		first_.offset = bytesWrittenFirstBuf;

		bufferHeaderSize_ = 0;
		bufferPos_ = bytesPerFrame_;
		bufferValidBytes_ = bytesWrittenFirstBuf - bufferPos_;
	}

	if (codecType_ == PSP_MODE_AT_3 || codecType_ == PSP_MODE_AT_3_PLUS) {
		SeekToSample(sample);
	}

	WriteContextToPSPMem();
	return 0;
}

void Atrac::InitLowLevel(u32 paramsAddr, bool jointStereo) {
	channels_ = Memory::Read_U32(paramsAddr);
	outputChannels_ = Memory::Read_U32(paramsAddr + 4);
	bufferMaxSize_ = Memory::Read_U32(paramsAddr + 8);
	bytesPerFrame_ = bufferMaxSize_;
	first_.writableBytes = bytesPerFrame_;
	ResetData();

	if (codecType_ == PSP_MODE_AT_3) {
		bitrate_ = (bytesPerFrame_ * 352800) / 1000;
		bitrate_ = (bitrate_ + 511) >> 10;
		jointStereo_ = false;
	} else if (codecType_ == PSP_MODE_AT_3_PLUS) {
		bitrate_ = (bytesPerFrame_ * 352800) / 1000;
		bitrate_ = ((bitrate_ >> 11) + 8) & 0xFFFFFFF0;
		jointStereo_ = false;
	}

	dataOff_ = 0;
	first_.size = 0;
	first_.filesize = bytesPerFrame_;
	bufferState_ = ATRAC_STATUS_LOW_LEVEL;
	currentSample_ = 0;
	CreateDecoder();
	WriteContextToPSPMem();
}
