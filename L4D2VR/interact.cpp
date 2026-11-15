#include "interact.h"
#include "game.h"
#include "haptics.h"
#include "hooks.h"
#include "netvars.h"
#include "offsets.h"
#include "trace.h"
#include "vr.h"
#include <chrono>
#include <cstring>
#include <mutex>

namespace
{
	constexpr unsigned int kTouchMask = 0x200400B | 0x4000000;
	constexpr int kUseToggle = 3;
	constexpr int kClassnameOffset = 0x60;
	constexpr int kUseSlotDisplacement = 0x22;
	constexpr int kBumpSlotDisplacement = 0x60;
	constexpr int kAbsOriginOffset = 0x1cc;
	constexpr float kGiveDistance = 0.01f;
	constexpr float kHandExtent = 2.5f;
	constexpr float kGunExtent = 1.5f;
	constexpr float kMaxSweep = 24.0f;
	constexpr double kServerFresh = 1.0;
	constexpr double kReachWindow = 0.4;
	constexpr double kGrabSettle = 0.5;

	using tDefaultTouch = void(__fastcall*)(void* weapon, void* edx, void* other);
	using tBumpWeapon = bool(__fastcall*)(void* player, void* edx, void* weapon);
	using tUse = void(__fastcall*)(void* entity, void* edx, void* activator, void* caller, int useType, float value);

	tDefaultTouch g_DefaultTouch = nullptr;
	Hook<tBumpWeapon> hkBumpWeapon;
	int g_BumpSlot = -1;
	bool g_BumpHooked = false;

	Game* g_Game_ = nullptr;
	VR* g_VR_ = nullptr;
	IEngineTrace* g_ServerTrace = nullptr;
	int g_UseSlot = -1;
	bool g_Enabled = true;

	std::mutex g_InputLock;
	Interact::Input g_Input;

	struct Probe
	{
		bool primed = false;
		Vector last = { 0, 0, 0 };
		void* pressed = nullptr;
	};

	Probe g_Probes[3];
	bool g_PrevGrip[2] = { false, false };
	bool g_ClientPrevGrip[2] = { false, false };
	Interact::Hand g_HoldHand = Interact::Right;
	double g_GrabAt = -10;
	void* g_ServerPlayer = nullptr;
	double g_LastServerTick = -10;
	void* g_ReachedWeapon = nullptr;
	double g_ReachedAt = -10;
	bool g_Granting = false;

	double Now()
	{
		using namespace std::chrono;
		return duration<double>(steady_clock::now().time_since_epoch()).count();
	}

	class TouchFilter : public CTraceFilter
	{
	public:
		explicit TouchFilter(void* player) : CTraceFilter(reinterpret_cast<IHandleEntity*>(player), 0) {}

		bool ShouldHitEntity(IHandleEntity* entity, int) override
		{
			return entity && entity != m_pPassEnt;
		}

		TraceType GetTraceType() const override
		{
			return TraceType::TRACE_ENTITIES_ONLY;
		}
	};

	const char* Classname(void* entity)
	{
		const char* name = *reinterpret_cast<const char**>(reinterpret_cast<char*>(entity) + kClassnameOffset);
		return name ? name : "";
	}

	bool StartsWith(const char* text, const char* prefix)
	{
		return std::strncmp(text, prefix, std::strlen(prefix)) == 0;
	}

	bool Pressable(const char* classname)
	{
		return std::strstr(classname, "button") && !std::strstr(classname, "floor");
	}

	bool Weapon(const char* classname)
	{
		return StartsWith(classname, "weapon_");
	}

	void Use(void* entity, void* player)
	{
		if (g_UseSlot < 0)
			return;
		tUse use = reinterpret_cast<tUse>((*reinterpret_cast<void***>(entity))[g_UseSlot]);
		use(entity, nullptr, player, player, kUseToggle, 0.0f);
	}

	void* Touching(Probe& probe, const Vector& position, float extent, void* player)
	{
		Vector start = probe.primed ? probe.last : position;
		Vector step = position - start;
		if (step.x * step.x + step.y * step.y + step.z * step.z > kMaxSweep * kMaxSweep)
			start = position;
		probe.last = position;
		probe.primed = true;

		Ray_t ray;
		ray.Init(start, position, Vector(-extent, -extent, -extent), Vector(extent, extent, extent));
		TouchFilter filter(player);
		CGameTrace trace;
		g_ServerTrace->TraceRay(ray, kTouchMask, &filter, &trace);
		if (!trace.DidHit() || !trace.m_pEnt || std::strcmp(Classname(trace.m_pEnt), "worldspawn") == 0)
			return nullptr;
		return trace.m_pEnt;
	}

	bool Holding()
	{
		static int heldOffset = Netvars::Find("class:CPortal_Player", "m_hAttachedObject");
		if (!heldOffset || !g_Game_)
			return false;
		void* player = g_Game_->GetClientEntity(g_Game_->m_EngineClient->GetLocalPlayer());
		if (!player)
			return false;
		int held = Netvars::At<int>(player, heldOffset);
		return held != -1 && held != 0;
	}

	void GrantWeapon(void* weapon, void* player)
	{
		if (!g_DefaultTouch)
			return;
		g_Granting = true;
		g_DefaultTouch(weapon, nullptr, player);
		g_Granting = false;
	}

	Vector AbsOrigin(void* entity)
	{
		return *reinterpret_cast<Vector*>(reinterpret_cast<char*>(entity) + kAbsOriginOffset);
	}

	bool Given(void* player, void* weapon)
	{
		Vector gap = AbsOrigin(weapon) - AbsOrigin(player);
		return gap.x * gap.x + gap.y * gap.y + gap.z * gap.z < kGiveDistance * kGiveDistance;
	}

	bool __fastcall dBumpWeapon(void* player, void* edx, void* weapon)
	{
		bool gated = g_Enabled && !g_Granting && weapon && player == g_ServerPlayer
			&& g_VR_ && g_VR_->m_IsVREnabled && Now() - g_LastServerTick < kServerFresh
			&& !Given(player, weapon);
		if (gated && !(weapon == g_ReachedWeapon && Now() - g_ReachedAt < kReachWindow))
			return false;
		return hkBumpWeapon.fOriginal(player, edx, weapon);
	}

	void HookBumpWeapon(void* player)
	{
		if (g_BumpHooked || g_BumpSlot < 0)
			return;
		g_BumpHooked = true;
		void* target = (*reinterpret_cast<void***>(player))[g_BumpSlot];
		if (hkBumpWeapon.createHook(target, &dBumpWeapon) == 0)
			hkBumpWeapon.enableHook();
	}

	void Pulse(int probe, float seconds, float frequency, float amplitude)
	{
		Haptics::Target target = probe == Interact::Left ? Haptics::LeftHand
			: probe == Interact::Right ? Haptics::RightHand : Haptics::GunHand;
		Haptics::Pulse(target, seconds, frequency, amplitude);
	}
}

namespace Interact
{
	void Install(Game* game, VR* vr)
	{
		g_Game_ = game;
		g_VR_ = vr;
		g_ServerTrace = static_cast<IEngineTrace*>(game->GetInterface("engine.dll", "EngineTraceServer004"));

		const uint8_t* inputUse = reinterpret_cast<const uint8_t*>(game->m_Offsets->InputUse.address);
		if (inputUse)
		{
			int displacement = *reinterpret_cast<const int*>(inputUse + kUseSlotDisplacement);
			if (displacement > 0 && displacement % 4 == 0 && displacement < 0x1000)
				g_UseSlot = displacement / 4;
		}

		const uint8_t* defaultTouch = reinterpret_cast<const uint8_t*>(game->m_Offsets->DefaultTouch.address);
		if (defaultTouch)
		{
			g_DefaultTouch = reinterpret_cast<tDefaultTouch>(game->m_Offsets->DefaultTouch.address);
			int displacement = *reinterpret_cast<const int*>(defaultTouch + kBumpSlotDisplacement);
			if (displacement > 0 && displacement % 4 == 0 && displacement < 0x1000)
				g_BumpSlot = displacement / 4;
		}

		std::cout << "Interact: use slot " << g_UseSlot << ", bump slot " << g_BumpSlot << ", server trace " << (g_ServerTrace ? "ok" : "missing") << "\n";
	}

	void SetEnabled(bool enabled)
	{
		g_Enabled = enabled;
	}

	void Publish(const Input& input)
	{
		std::lock_guard<std::mutex> lock(g_InputLock);
		g_Input = input;
	}

	bool Physical()
	{
		return g_Enabled && g_ServerTrace && g_UseSlot >= 0 && Now() - g_LastServerTick < kServerFresh;
	}

	Hand HoldHand()
	{
		return g_HoldHand;
	}

	void ServerTick(void* serverPlayer)
	{
		if (!g_Enabled || !g_ServerTrace || g_UseSlot < 0 || !serverPlayer)
			return;

		Input in;
		{
			std::lock_guard<std::mutex> lock(g_InputLock);
			in = g_Input;
		}
		if (!in.valid)
			return;

		HookBumpWeapon(serverPlayer);

		if (serverPlayer != g_ServerPlayer)
		{
			for (Probe& probe : g_Probes)
				probe = Probe();
			g_ServerPlayer = serverPlayer;
		}
		g_LastServerTick = Now();

		bool holding = Holding();
		if (!holding && Now() - g_GrabAt > kGrabSettle)
			g_HoldHand = Right;

		for (int h = 0; h < 2; ++h)
		{
			const HandState& hand = in.hand[h];
			Probe& probe = g_Probes[h];
			bool gripPressed = hand.grip && !g_PrevGrip[h];
			g_PrevGrip[h] = hand.grip;

			if (!hand.tracked)
			{
				probe = Probe();
				continue;
			}

			void* touched = Touching(probe, hand.position + hand.forward * 3.0f, kHandExtent, serverPlayer);
			if (!touched)
			{
				probe.pressed = nullptr;
				continue;
			}

			const char* classname = Classname(touched);
			if (Weapon(classname))
			{
				g_ReachedWeapon = touched;
				g_ReachedAt = Now();
				GrantWeapon(touched, serverPlayer);
				if (probe.pressed != touched)
				{
					probe.pressed = touched;
					Pulse(h, 0.06f, 120.0f, 0.5f);
				}
				continue;
			}

			if (Pressable(classname))
			{
				if (probe.pressed != touched)
				{
					probe.pressed = touched;
					Use(touched, serverPlayer);
					Pulse(h, 0.04f, 160.0f, 0.6f);
				}
				continue;
			}
			probe.pressed = nullptr;

			if (gripPressed && !holding)
			{
				Use(touched, serverPlayer);
				g_HoldHand = static_cast<Hand>(h);
				g_GrabAt = Now();
				holding = true;
				Pulse(h, 0.05f, 120.0f, 0.45f);
			}
		}

		Probe& gun = g_Probes[2];
		if (in.hand[Right].tracked)
		{
			void* touched = Touching(gun, in.gunTip, kGunExtent, serverPlayer);
			if (touched && Pressable(Classname(touched)))
			{
				if (gun.pressed != touched)
				{
					gun.pressed = touched;
					Use(touched, serverPlayer);
					Pulse(2, 0.04f, 160.0f, 0.6f);
				}
			}
			else
				gun.pressed = nullptr;
		}
		else
			gun = Probe();
	}

	bool WantsUse(bool classicPressed)
	{
		if (!Physical())
			return classicPressed;

		Input in;
		{
			std::lock_guard<std::mutex> lock(g_InputLock);
			in = g_Input;
		}

		bool released = false;
		for (int h = 0; h < 2; ++h)
		{
			if (h == g_HoldHand && g_ClientPrevGrip[h] && !in.hand[h].grip)
				released = true;
			g_ClientPrevGrip[h] = in.hand[h].grip;
		}

		if (!Holding())
			return false;
		return released;
	}
}
