#pragma once
#include <string>
#include <unordered_map>

class IBaseClientDLL;

namespace Netvars
{
	void Init(IBaseClientDLL* client);
	int Get(const char* table, const char* prop);
	int Find(const char* table, const char* prop);
	bool Ready();

	template <typename T>
	inline T& At(void* entity, int offset)
	{
		return *reinterpret_cast<T*>(reinterpret_cast<char*>(entity) + offset);
	}
}
