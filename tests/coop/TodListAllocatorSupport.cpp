#include "../../src/Sexy.TodLib/TodDebug.h"
#include "../../src/Sexy.TodLib/TodList.h"

#include <cstdlib>
#include <new>
#include <stdexcept>

void* TodMalloc(int theSize)
{
	void* block = std::malloc(static_cast<std::size_t>(theSize));
	if (block == nullptr)
		throw std::bad_alloc();
	return block;
}

void TodFree(void* theBlock)
{
	std::free(theBlock);
}

void TodAssertFailed(const char*, const char*, int, const char*, ...)
{
	throw std::runtime_error("TodAllocator assertion failed in cooperative test");
}

TodAllocator* FindGlobalAllocator(int)
{
	return nullptr;
}
