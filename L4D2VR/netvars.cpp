#include "netvars.h"
#include "game.h"
#include <cstring>
#include <Windows.h>
#include <psapi.h>
#include <cstdint>
#include <iostream>

namespace
{
	struct RecvTable;

	struct RecvProp
	{
		char* m_pVarName;
		int m_RecvType;
		int m_Flags;
		int m_StringBufferSize;
		bool m_bInsideArray;
		const void* m_pExtraData;
		RecvProp* m_pArrayProp;
		void* m_ArrayLengthProxy;
		void* m_ProxyFn;
		void* m_DataTableProxyFn;
		RecvTable* m_pDataTable;
		int m_Offset;
		int m_ElementStride;
		int m_nElements;
		const char* m_pParentArrayPropName;
	};

	struct RecvTable
	{
		RecvProp* m_pProps;
		int m_nProps;
		void* m_pDecoder;
		char* m_pNetTableName;
		bool m_bInitialized;
		bool m_bInMainList;
	};

	struct ClientClass
	{
		void* m_pCreateFn;
		void* m_pCreateEventFn;
		char* m_pNetworkName;
		RecvTable* m_pRecvTable;
		ClientClass* m_pNext;
		int m_ClassID;
	};

	constexpr int DPT_DataTable = 6;

	std::unordered_map<std::string, std::unordered_map<std::string, int>> g_Tables;
	bool g_Ready = false;

	void Walk(RecvTable* table, int base, std::unordered_map<std::string, int>& flat, int depth)
	{
		if (!table || depth > 16)
			return;

		auto& own = g_Tables[table->m_pNetTableName];

		for (int i = 0; i < table->m_nProps; ++i)
		{
			RecvProp& prop = table->m_pProps[i];
			if (!prop.m_pVarName)
				continue;

			if (own.find(prop.m_pVarName) == own.end())
				own[prop.m_pVarName] = prop.m_Offset;

			int absolute = base + prop.m_Offset;
			if (flat.find(prop.m_pVarName) == flat.end())
				flat[prop.m_pVarName] = absolute;

			if (prop.m_RecvType == DPT_DataTable && prop.m_pDataTable)
				Walk(prop.m_pDataTable, absolute, flat, depth + 1);
		}
	}
}

namespace Netvars
{
	void Init(IBaseClientDLL* client)
	{
		using GetAllClassesFn = ClientClass*(__thiscall*)(void*);
		void** vtable = *reinterpret_cast<void***>(client);

		HMODULE module = GetModuleHandleA("client.dll");
		MODULEINFO info = {};
		GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info));
		uintptr_t lo = reinterpret_cast<uintptr_t>(info.lpBaseOfDll);
		uintptr_t hi = lo + info.SizeOfImage;
		auto inModule = [&](const void* p) { return reinterpret_cast<uintptr_t>(p) >= lo && reinterpret_cast<uintptr_t>(p) < hi; };

		ClientClass* head = nullptr;
		for (int i = 0; i < 16 && !head; ++i)
		{
			const uint8_t* code = static_cast<const uint8_t*>(vtable[i]);
			if (!inModule(code) || code[0] != 0xA1 || code[5] != 0xC3)
				continue;
			ClientClass* candidate = reinterpret_cast<GetAllClassesFn>(vtable[i])(client);
			if (candidate && inModule(candidate) && inModule(candidate->m_pNetworkName) && inModule(candidate->m_pRecvTable)
				&& inModule(candidate->m_pRecvTable->m_pNetTableName) && std::strncmp(candidate->m_pRecvTable->m_pNetTableName, "DT_", 3) == 0)
				head = candidate;
		}

		for (ClientClass* cc = head; cc; cc = cc->m_pNext)
		{
			if (!cc->m_pRecvTable)
				continue;

			std::unordered_map<std::string, int> flat;
			Walk(cc->m_pRecvTable, 0, flat, 0);
			g_Tables[std::string("class:") + cc->m_pNetworkName] = std::move(flat);
		}

		g_Ready = !g_Tables.empty();
		std::cout << "Netvars: " << g_Tables.size() << " tables\n";
		if (!g_Ready)
			Game::errorMsg("Couldn't read the game's network tables, so the VR body and cutscene camera are turned off.");
	}

	int Find(const char* table, const char* prop)
	{
		auto t = g_Tables.find(table);
		if (t == g_Tables.end())
			return 0;
		auto p = t->second.find(prop);
		return p == t->second.end() ? 0 : p->second;
	}

	bool Ready()
	{
		return g_Ready;
	}

	int Get(const char* table, const char* prop)
	{
		auto t = g_Tables.find(table);
		if (t == g_Tables.end())
		{
			Game::errorMsg((std::string("Missing netvar table ") + table).c_str());
			return 0;
		}

		auto p = t->second.find(prop);
		if (p == t->second.end())
		{
			Game::errorMsg((std::string("Missing netvar ") + table + "." + prop).c_str());
			return 0;
		}

		return p->second;
	}
}
