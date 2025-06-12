#pragma once

#include "CoreMinimal.h"

class FSuzieDecompressionHelper
{
public:
	/** Decompresses memory with Gzip */
	static bool DecompressMemoryGzip(const TArray<uint8>& CompressedData, TArray<uint8>& OutDecompressedData);
private:
	static void* ZlibAlloc(void* opaque, unsigned int size, unsigned int num);
	static void ZlibFree(void* opaque, void* p);
};
