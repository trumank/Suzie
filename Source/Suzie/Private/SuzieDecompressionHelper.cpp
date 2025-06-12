#include "SuzieDecompressionHelper.h"

THIRD_PARTY_INCLUDES_START
#include "zlib.h"
THIRD_PARTY_INCLUDES_END

bool FSuzieDecompressionHelper::DecompressMemoryGzip(const TArray<uint8>& CompressedData, TArray<uint8>& OutDecompressedData)
{
	z_stream GzipStream;
	GzipStream.zalloc = &FSuzieDecompressionHelper::ZlibAlloc;
	GzipStream.zfree = &FSuzieDecompressionHelper::ZlibFree;
	GzipStream.opaque = nullptr;

	// Setup input buffer
	GzipStream.next_in = (uint8*)CompressedData.GetData();
	GzipStream.avail_in = CompressedData.Num();

	// Init deflate settings to use GZIP
	constexpr int32 GzipStreamEncoding = 16;
	inflateInit2(&GzipStream, MAX_WBITS | GzipStreamEncoding);

	uint8 LocalDecompressionBuffer[4096];
	GzipStream.next_out = LocalDecompressionBuffer;
	GzipStream.avail_out = sizeof(LocalDecompressionBuffer);

	int32 InflateStatusCode;
	while ((InflateStatusCode = inflate(&GzipStream, Z_SYNC_FLUSH)) == Z_OK)
	{
		// Append the number of bytes decompressed to the output
		const int32 BytesWritten = sizeof(LocalDecompressionBuffer) - GzipStream.avail_out;
		OutDecompressedData.Append(LocalDecompressionBuffer, BytesWritten);

		// Reset the next byte to the start of the buffer and available space to the entire buffer
		GzipStream.next_out = LocalDecompressionBuffer;
		GzipStream.avail_out = sizeof(LocalDecompressionBuffer);
	}
	inflateEnd(&GzipStream);

	// Append final decompressed bytes to the output
	const int32 BytesWritten = sizeof(LocalDecompressionBuffer) - GzipStream.avail_out;
	OutDecompressedData.Append(LocalDecompressionBuffer, BytesWritten);
	
	return InflateStatusCode == Z_STREAM_END;
}

void* FSuzieDecompressionHelper::ZlibAlloc(void*, unsigned int size, unsigned int num)
{
	return FMemory::Malloc(size * num);
}

void FSuzieDecompressionHelper::ZlibFree(void*, void* p)
{
	FMemory::Free(p);
}
