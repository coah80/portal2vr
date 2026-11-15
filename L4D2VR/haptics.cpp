#include "haptics.h"
#include "game.h"
#include "hooks.h"
#include "netvars.h"
#include "offsets.h"
#include "vr.h"
#include <algorithm>
#include <chrono>
#include <mutex>

namespace
{
	using tRumbleEffect = void(__cdecl*)(unsigned char userID, unsigned char effect, unsigned char data, unsigned char flags);
	Hook<tRumbleEffect> hkRumbleEffect;

	enum RumbleFlag
	{
		FlagStop = 0x01,
		FlagLoop = 0x02,
		FlagUpdateScale = 0x08,
		FlagInitialScale = 0x40,
	};

	enum Effect
	{
		StopAll = 0,
		WeaponFirst = 1,
		WeaponLast = 7,
		Rpg = 8,
		CrowbarSwing = 9,
		AirboatGun = 10,
		EngineLoop = 11,
		FlatLeft = 12,
		FlatRight = 13,
		FlatBoth = 14,
		DamageLow = 15,
		DamageMedium = 16,
		DamageHigh = 17,
		FallLong = 18,
		FallShort = 19,
		GrabOpen = 20,
		GrabPunt = 21,
		HoldLow = 22,
		HoldMedium = 23,
		HoldHigh = 24,
		PortalLeft = 25,
		PortalRight = 26,
		PlacementFailure = 27,
		EffectCount
	};

	struct Loop
	{
		bool on = false;
		Haptics::Target target = Haptics::GunHand;
		float frequency = 0;
		float amplitude = 0;
	};

	Game* g_Game_ = nullptr;
	VR* g_VR_ = nullptr;
	vr::VRActionHandle_t g_LeftAction = vr::k_ulInvalidActionHandle;
	vr::VRActionHandle_t g_RightAction = vr::k_ulInvalidActionHandle;
	float g_Strength = 1.0f;

	std::mutex g_LoopLock;
	Loop g_Loops[EffectCount];

	double g_LastFallRumble = -10;
	bool g_HaveState = false;
	int g_PrevFlags = 0;
	float g_PrevFall = 0;
	int g_PrevHeld = -1;

	double Now()
	{
		using namespace std::chrono;
		return duration<double>(steady_clock::now().time_since_epoch()).count();
	}

	int NetvarOffset(const char* prop)
	{
		return Netvars::Find("class:CPortal_Player", prop);
	}

	void Emit(vr::VRActionHandle_t action, float seconds, float frequency, float amplitude, float delay)
	{
		if (action == vr::k_ulInvalidActionHandle || !g_VR_ || !g_VR_->m_Input)
			return;
		float scaled = std::clamp(amplitude * g_Strength, 0.0f, 1.0f);
		if (scaled <= 0.0f)
			return;
		g_VR_->m_Input->TriggerHapticVibrationAction(action, delay, seconds, frequency, scaled, vr::k_ulInvalidInputValueHandle);
	}

	void StartLoop(int effect, Haptics::Target target, float frequency, float amplitude)
	{
		std::lock_guard<std::mutex> lock(g_LoopLock);
		g_Loops[effect] = { true, target, frequency, amplitude };
	}

	void StopLoop(int effect)
	{
		std::lock_guard<std::mutex> lock(g_LoopLock);
		g_Loops[effect].on = false;
	}

	void StopAllLoops()
	{
		std::lock_guard<std::mutex> lock(g_LoopLock);
		for (Loop& loop : g_Loops)
			loop.on = false;
	}

	void Play(int effect, int data, int flags)
	{
		using namespace Haptics;

		if (effect <= StopAll)
		{
			StopAllLoops();
			return;
		}
		if (effect >= EffectCount)
			return;
		if (flags & FlagStop)
		{
			StopLoop(effect);
			return;
		}

		float scale = (flags & (FlagInitialScale | FlagUpdateScale)) && data > 0 ? data / 100.0f : 1.0f;
		if (flags & FlagUpdateScale)
		{
			std::lock_guard<std::mutex> lock(g_LoopLock);
			if (g_Loops[effect].on)
			{
				g_Loops[effect].amplitude *= scale;
				return;
			}
		}

		bool loop = (flags & FlagLoop) != 0;

		switch (effect)
		{
		case PortalLeft:
		case PortalRight:
			Pulse(GunHand, 0.025f, 180.0f, 0.75f);
			Pulse(GunHand, 0.06f, 70.0f, 0.35f, 0.025f);
			break;
		case PlacementFailure:
			Pulse(GunHand, 0.035f, 100.0f, 0.5f);
			Pulse(GunHand, 0.035f, 100.0f, 0.5f, 0.075f);
			break;
		case FallLong:
			g_LastFallRumble = Now();
			Pulse(BothHands, 0.3f, 45.0f, 1.0f);
			break;
		case FallShort:
			g_LastFallRumble = Now();
			Pulse(BothHands, 0.14f, 55.0f, 0.7f);
			break;
		case DamageLow:
			Pulse(BothHands, 0.07f, 70.0f, 0.45f);
			break;
		case DamageMedium:
			Pulse(BothHands, 0.12f, 70.0f, 0.65f);
			break;
		case DamageHigh:
			Pulse(BothHands, 0.2f, 60.0f, 0.9f);
			break;
		case GrabOpen:
			Pulse(GunHand, 0.05f, 140.0f, 0.4f);
			break;
		case GrabPunt:
			Pulse(GunHand, 0.1f, 80.0f, 0.8f);
			break;
		case HoldLow:
		case HoldMedium:
		case HoldHigh:
		{
			float amplitude = effect == HoldLow ? 0.12f : effect == HoldMedium ? 0.2f : 0.3f;
			if (loop)
				StartLoop(effect, GunHand, 45.0f, amplitude * scale);
			else
				Pulse(GunHand, 0.08f, 45.0f, amplitude * scale);
			break;
		}
		case FlatLeft:
		case FlatRight:
		case FlatBoth:
		{
			Target target = effect == FlatLeft ? LeftHand : effect == FlatRight ? RightHand : BothHands;
			if (loop)
				StartLoop(effect, target, 60.0f, 0.5f * scale);
			else
				Pulse(target, 0.1f, 60.0f, 0.5f * scale);
			break;
		}
		case EngineLoop:
			if (loop)
				StartLoop(effect, BothHands, 30.0f, 0.2f * scale);
			else
				Pulse(BothHands, 0.1f, 30.0f, 0.2f * scale);
			break;
		case Rpg:
			Pulse(BothHands, 0.15f, 50.0f, 0.7f * scale);
			break;
		default:
			if (loop)
				StartLoop(effect, GunHand, 90.0f, 0.4f * scale);
			else
				Pulse(GunHand, 0.06f, 90.0f, 0.6f * scale);
			break;
		}
	}

	void __cdecl dRumbleEffect(unsigned char userID, unsigned char effect, unsigned char data, unsigned char flags)
	{
		if (g_VR_ && g_VR_->m_IsVREnabled)
			Play(effect, data, flags);
		hkRumbleEffect.fOriginal(userID, effect, data, flags);
	}
}

namespace Haptics
{
	void Install(Game* game, VR* vr)
	{
		g_Game_ = game;
		g_VR_ = vr;

		vr->m_Input->GetActionHandle("/actions/base/out/vibration_left", &g_LeftAction);
		vr->m_Input->GetActionHandle("/actions/base/out/vibration_right", &g_RightAction);

		hkRumbleEffect.createHook((LPVOID)game->m_Offsets->RumbleEffect.address, &dRumbleEffect);
		hkRumbleEffect.enableHook();
	}

	void SetStrength(float strength)
	{
		g_Strength = std::clamp(strength, 0.0f, 2.0f);
	}

	void Pulse(Target target, float seconds, float frequency, float amplitude, float delay)
	{
		bool leftHanded = g_VR_ && g_VR_->m_LeftHanded;
		vr::VRActionHandle_t gun = leftHanded ? g_LeftAction : g_RightAction;
		vr::VRActionHandle_t off = leftHanded ? g_RightAction : g_LeftAction;

		switch (target)
		{
		case LeftHand: Emit(g_LeftAction, seconds, frequency, amplitude, delay); break;
		case RightHand: Emit(g_RightAction, seconds, frequency, amplitude, delay); break;
		case GunHand: Emit(gun, seconds, frequency, amplitude, delay); break;
		case OffHand: Emit(off, seconds, frequency, amplitude, delay); break;
		case BothHands:
			Emit(g_LeftAction, seconds, frequency, amplitude, delay);
			Emit(g_RightAction, seconds, frequency, amplitude, delay);
			break;
		}
	}

	void Frame(float seconds)
	{
		Loop loops[EffectCount];
		{
			std::lock_guard<std::mutex> lock(g_LoopLock);
			std::copy(std::begin(g_Loops), std::end(g_Loops), loops);
		}
		float span = std::clamp(seconds * 1.5f, 0.011f, 0.05f);
		for (const Loop& loop : loops)
			if (loop.on)
				Pulse(loop.target, span, loop.frequency, loop.amplitude);
	}

	void Think(void* player)
	{
		if (!player || !Netvars::Ready())
			return;

		static int flagsOffset = NetvarOffset("m_fFlags");
		static int fallOffset = NetvarOffset("m_flFallVelocity");
		static int heldOffset = NetvarOffset("m_hAttachedObject");

		int flags = flagsOffset ? Netvars::At<int>(player, flagsOffset) : 0;
		float fall = fallOffset ? Netvars::At<float>(player, fallOffset) : 0.0f;
		int held = heldOffset ? Netvars::At<int>(player, heldOffset) : -1;
		bool holding = held != -1 && held != 0;

		if (g_HaveState)
		{
			bool landed = !(g_PrevFlags & 1) && (flags & 1);
			if (landed && g_PrevFall > 120.0f && Now() - g_LastFallRumble > 0.25)
			{
				float amplitude = std::clamp((g_PrevFall - 120.0f) / 480.0f, 0.12f, 0.8f);
				Pulse(BothHands, 0.04f + 0.08f * amplitude, 55.0f, amplitude);
			}

			bool wasHolding = g_PrevHeld != -1 && g_PrevHeld != 0;
			if (holding && !wasHolding)
				Pulse(GunHand, 0.05f, 120.0f, 0.45f);
			else if (!holding && wasHolding)
				Pulse(GunHand, 0.04f, 90.0f, 0.3f);
		}

		g_HaveState = true;
		g_PrevFlags = flags;
		g_PrevFall = fall;
		g_PrevHeld = held;
	}

	void OnPortalled()
	{
		Pulse(BothHands, 0.07f, 220.0f, 0.22f);
	}
}
