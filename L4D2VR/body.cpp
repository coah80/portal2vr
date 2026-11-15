#include "body.h"
#include "game.h"
#include "hooks.h"
#include "netvars.h"
#include "offsets.h"
#include "sdk.h"
#include "vr.h"
#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

namespace
{
	using rig::V3;
	using rig::M34;

	constexpr int kBoneCacheOffset = 0xD48;
	constexpr int kBoneCountOffset = 0xD54;
	constexpr int kStudioHdrOffset = 0xD84;
	constexpr int kBoneCounterOffset = 0xAF8;
	constexpr int kStudioNumBones = 0x9C;
	constexpr int kStudioBoneIndex = 0xA0;
	constexpr int kStudioEyeZ = 0x58;
	constexpr int kStudioBoneStride = 216;
	constexpr int kBoneUsedByVertexLod0 = 0x400;
	constexpr int kBoneUsedByAnything = 0x7FF00;
	constexpr unsigned kShadowPassFlags = 0x60000000;
	constexpr float kGameEyeHeight = 64.0f;

	using tBuildTransformations = void(__thiscall*)(void* entity, void* hdr, void* pos, void* q, const void* cameraTransform, int boneMask, void* computed);
	using tSetupBones = bool(__thiscall*)(void* renderable, void* boneToWorldOut, int maxBones, int boneMask, float currentTime);
	using tDrawModel = int(__thiscall*)(void* renderable, int flags, const void* instance);
	using tShouldSkip = bool(__thiscall*)(void* player);

	Hook<tBuildTransformations> hkBuildTransformations;
	Hook<tSetupBones> hkSetupBones;
	Hook<tDrawModel> hkDrawModel;
	Hook<tShouldSkip> hkShouldSkip;

	Game* g_Game_ = nullptr;
	VR* g_VR_ = nullptr;

	std::mutex g_InputLock;
	Body::Input g_Input;
	bool g_Enabled = true;
	bool g_Hidden = false;

	uintptr_t g_PortalRenderPtr = 0;
	int g_RecursionLevelOffset = 0xBA0;

	thread_local bool t_DrawingPlayer = false;
	thread_local unsigned t_DrawFlags = 0;
	thread_local bool t_InViewmodelSetup = false;

	int g_LastSolvedCounter = -1;

	struct Mapping
	{
		bool ok = false;
		rig::BodyBones bones;
		std::vector<int> parents;
		float modelEye = 70.0f;
	};

	std::unordered_map<const void*, Mapping> g_Mappings;
	std::unordered_map<const void*, int> g_ViewmodelHands;

	struct StudioBone
	{
		const char* name;
		int parent;
	};

	const char* StudioHdrOf(void* entity)
	{
		void* cstudio = *reinterpret_cast<void**>(reinterpret_cast<char*>(entity) + kStudioHdrOffset);
		if (!cstudio)
			return nullptr;
		return *reinterpret_cast<const char**>(cstudio);
	}

	int NumBones(const char* studio)
	{
		return *reinterpret_cast<const int*>(studio + kStudioNumBones);
	}

	StudioBone BoneAt(const char* studio, int index)
	{
		const char* bone = studio + *reinterpret_cast<const int*>(studio + kStudioBoneIndex) + index * kStudioBoneStride;
		return { bone + *reinterpret_cast<const int*>(bone), *reinterpret_cast<const int*>(bone + 4) };
	}

	int FindBone(const char* studio, const char* name)
	{
		int count = NumBones(studio);
		for (int i = 0; i < count; ++i)
			if (_stricmp(BoneAt(studio, i).name, name) == 0)
				return i;
		return -1;
	}

	const Mapping& MappingFor(const char* studio)
	{
		auto found = g_Mappings.find(studio);
		if (found != g_Mappings.end())
			return found->second;

		Mapping& m = g_Mappings[studio];
		int count = NumBones(studio);
		m.parents.resize(count);
		for (int i = 0; i < count; ++i)
			m.parents[i] = BoneAt(studio, i).parent;

		rig::BodyBones& b = m.bones;
		b.root = FindBone(studio, "root");
		b.neck = FindBone(studio, "neck1");
		b.head = FindBone(studio, "head");
		for (const char* hide : { "head", "neck_twist", "hair_jiggle_bone_0", "hair_jiggle_bone_1" })
		{
			int bone = FindBone(studio, hide);
			if (bone >= 0)
				b.headHide.push_back(bone);
		}

		static const char* fingers[rig::FingerCount] = { "thumb", "index", "mid", "ring", "pinky" };
		for (int s = 0; s < 2; ++s)
		{
			std::string side = s == rig::Right ? "R" : "L";
			rig::ArmBones& a = b.arm[s];
			a.clavicle = FindBone(studio, ("clavicle_" + side).c_str());
			a.upper = FindBone(studio, ("bicep_" + side).c_str());
			a.upperTwist = FindBone(studio, (side + "upperArmTwistJNT").c_str());
			a.elbow = FindBone(studio, ("elbow_" + side).c_str());
			a.wrist = FindBone(studio, ("wrist_" + side).c_str());
			for (int t = 0; t < 3; ++t)
				a.forearmTwist[t] = FindBone(studio, ("lowArm" + side + "_twist0_" + std::to_string(t)).c_str());
			for (int f = 0; f < rig::FingerCount; ++f)
				for (int j = 0; j < 3; ++j)
					a.finger[f][j] = FindBone(studio, (std::string(fingers[f]) + "_" + std::to_string(j) + "_" + side).c_str());

			b.leg[s].thigh = FindBone(studio, ("thigh_" + side).c_str());
			b.leg[s].knee = FindBone(studio, ("knee_" + side).c_str());
			b.leg[s].ankle = FindBone(studio, ("ankle_" + side).c_str());
		}

		m.ok = b.neck >= 0 && b.head >= 0 && b.arm[0].wrist >= 0 && b.arm[1].wrist >= 0 && b.arm[1].finger[rig::Middle][0] >= 0;
		float eye = *reinterpret_cast<const float*>(studio + kStudioEyeZ);
		if (eye > 20.0f && eye < 120.0f)
			m.modelEye = eye;
		return m;
	}

	void* LocalPlayer()
	{
		int index = g_Game_->m_EngineClient->GetLocalPlayer();
		return g_Game_->GetClientEntity(index);
	}

	void* EntityFromHandle(void* owner, const char* prop)
	{
		static std::unordered_map<std::string, int> offsets;
		auto known = offsets.find(prop);
		int offset = known != offsets.end() ? known->second : (offsets[prop] = Netvars::Find("class:CPortal_Player", prop));
		if (!offset)
			return nullptr;
		int handle = Netvars::At<int>(owner, offset);
		if (handle == -1 || handle == 0)
			return nullptr;
		return g_Game_->GetClientEntityFromHandle(handle);
	}

	M34* BoneCache(void* entity, int* count)
	{
		char* e = reinterpret_cast<char*>(entity);
		*count = *reinterpret_cast<int*>(e + kBoneCountOffset);
		return *reinterpret_cast<M34**>(e + kBoneCacheOffset);
	}

	V3 ToUnscaled(const V3& point, const V3& origin, float scale)
	{
		return origin + (point - origin) * (1.0f / scale);
	}

	V3 Tilt(const V3& forward, const V3& up, float degrees)
	{
		float r = degrees * 3.14159265f / 180.0f;
		return rig::Normalized(forward * std::cos(r) + up * std::sin(r));
	}

	bool ViewmodelHand(void* player, M34* hand)
	{
		void* vm = EntityFromHandle(player, "m_hViewModel[0]");
		if (!vm)
			return false;
		const char* studio = StudioHdrOf(vm);
		if (!studio)
			return false;

		auto cached = g_ViewmodelHands.find(studio);
		int bone = cached != g_ViewmodelHands.end() ? cached->second : (g_ViewmodelHands[studio] = FindBone(studio, "ValveBiped.Bip01_R_Hand"));
		if (bone < 0)
			return false;

		int count = 0;
		M34* cache = BoneCache(vm, &count);
		if (!cache || bone >= count)
			return false;
		*hand = cache[bone];
		return true;
	}

	void PrepareViewmodel(void* player, float currentTime)
	{
		void* vm = EntityFromHandle(player, "m_hViewModel[0]");
		if (!vm)
			return;
		t_InViewmodelSetup = true;
		hkSetupBones.fOriginal(reinterpret_cast<char*>(vm) + 4, nullptr, 128, kBoneUsedByAnything, currentTime);
		t_InViewmodelSetup = false;
	}

	void FillGunHand(rig::HandTarget& target, const M34& hand, const V3& origin, float scale)
	{
		target.valid = true;
		target.wristPosition = ToUnscaled(hand.Origin(), origin, scale);
		target.fingerDirection = rig::Normalized(hand.Axis(0));
		target.palmNormal = -rig::Normalized(hand.Axis(1));
	}

	void FillControllerHand(rig::HandTarget& target, const Body::HandInput& in, const V3& base, const V3& origin, float scale, rig::Hand side)
	{
		V3 position = base + in.position;
		V3 wrist = position - in.forward * 3.9f - in.up * 1.3f;
		target.valid = true;
		target.wristPosition = ToUnscaled(wrist, origin, scale);
		target.fingerDirection = Tilt(in.forward, in.up, -35.0f);
		target.palmNormal = side == rig::Right ? -in.right : in.right;
	}

	void ApplyCurls(rig::HandTarget& target, const Body::HandInput& in, bool onGun, float trigger)
	{
		static const float gunGrip[rig::FingerCount] = { 0.55f, 0.35f, 0.8f, 0.85f, 0.9f };
		static const float relaxed[rig::FingerCount] = { 0.25f, 0.2f, 0.3f, 0.35f, 0.4f };

		for (int f = 0; f < rig::FingerCount; ++f)
		{
			float base = onGun ? gunGrip[f] : relaxed[f];
			float curl = in.skeletal ? in.curl[f] : base;
			if (onGun && f >= rig::Middle)
				curl = std::max(curl, base * 0.85f);
			if (onGun && f == rig::Index && !in.skeletal)
				curl = base + 0.45f * trigger;
			target.curl[f] = std::clamp(curl, 0.0f, 1.0f);
		}
	}

	void Solve(void* player, const char* studio, const M34& toWorld)
	{
		const Mapping& map = MappingFor(studio);
		if (!map.ok)
			return;

		int count = 0;
		M34* cache = BoneCache(player, &count);
		int bones = NumBones(studio);
		if (!cache || count < bones)
			return;

		Body::Input in;
		{
			std::lock_guard<std::mutex> lock(g_InputLock);
			in = g_Input;
		}
		if (!in.valid)
			return;

		V3 base = { g_VR_->m_SetupOrigin.x, g_VR_->m_SetupOrigin.y, g_VR_->m_SetupOrigin.z };
		V3 origin = toWorld.Origin();
		float scale = kGameEyeHeight / map.modelEye;

		V3 eye = ToUnscaled(base + in.eyeOffset, origin, scale);
		V3 flatForward = rig::Normalized(V3{ in.hmdForward.x, in.hmdForward.y, 0 }, { 1, 0, 0 });

		rig::BodyTarget target;
		target.neckPosition = eye - in.hmdUp * 6.5f - in.hmdForward * 4.9f;
		target.bodyForward = flatForward;
		target.bodyUp = { 0, 0, 1 };

		M34 gun;
		bool onGun = ViewmodelHand(player, &gun);
		if (onGun)
			FillGunHand(target.hand[rig::Right], gun, origin, scale);
		else if (in.hand[rig::Right].tracked)
			FillControllerHand(target.hand[rig::Right], in.hand[rig::Right], base, origin, scale, rig::Right);
		ApplyCurls(target.hand[rig::Right], in.hand[rig::Right], onGun, in.trigger);

		if (in.hand[rig::Left].tracked)
			FillControllerHand(target.hand[rig::Left], in.hand[rig::Left], base, origin, scale, rig::Left);
		ApplyCurls(target.hand[rig::Left], in.hand[rig::Left], false, 0);

		rig::Pose pose;
		pose.Reset(cache, map.parents.data(), bones);
		rig::SolveBody(pose, map.bones, target);
		pose.CopyTo(cache);

		for (int i = 0; i < bones; ++i)
		{
			M34& m = cache[i];
			for (int a = 0; a < 3; ++a)
				m.SetAxis(a, m.Axis(a) * scale);
			m.SetOrigin(origin + (m.Origin() - origin) * scale);
		}
	}

	void __fastcall dBuildTransformations(void* ecx, void* edx, void* hdr, void* pos, void* q, const void* cameraTransform, int boneMask, void* computed)
	{
		hkBuildTransformations.fOriginal(ecx, hdr, pos, q, cameraTransform, boneMask, computed);

		if (!Body::Visible() || !(boneMask & kBoneUsedByVertexLod0) || !hdr)
			return;
		if (ecx != LocalPlayer())
			return;

		int counter = *reinterpret_cast<int*>(reinterpret_cast<char*>(ecx) + kBoneCounterOffset);
		if (counter == g_LastSolvedCounter)
			return;
		g_LastSolvedCounter = counter;

		const char* studio = *reinterpret_cast<const char**>(hdr);
		if (studio)
			Solve(ecx, studio, *reinterpret_cast<const M34*>(cameraTransform));
	}

	bool __fastcall dSetupBones(void* ecx, void* edx, void* out, int maxBones, int boneMask, float currentTime)
	{
		void* entity = reinterpret_cast<char*>(ecx) - 4;
		bool local = !t_InViewmodelSetup && Body::Visible() && entity == LocalPlayer();

		if (local)
			PrepareViewmodel(entity, currentTime);

		bool result = hkSetupBones.fOriginal(ecx, out, maxBones, boneMask, currentTime);

		if (local && result && out && t_DrawingPlayer && !(t_DrawFlags & kShadowPassFlags) && Body::ViewRecursionLevel() == 0)
		{
			const char* studio = StudioHdrOf(entity);
			if (studio)
			{
				const Mapping& map = MappingFor(studio);
				if (map.ok && maxBones >= NumBones(studio))
					rig::CollapseBones(reinterpret_cast<M34*>(out), map.bones);
			}
		}
		return result;
	}

	int __fastcall dDrawModel(void* ecx, void* edx, int flags, const void* instance)
	{
		bool wasDrawing = t_DrawingPlayer;
		unsigned wasFlags = t_DrawFlags;
		t_DrawingPlayer = true;
		t_DrawFlags = static_cast<unsigned>(flags);
		int result = hkDrawModel.fOriginal(ecx, flags, instance);
		t_DrawingPlayer = wasDrawing;
		t_DrawFlags = wasFlags;
		return result;
	}

	bool __fastcall dShouldSkip(void* ecx, void* edx)
	{
		if (Body::Visible())
			return false;
		return hkShouldSkip.fOriginal(ecx);
	}

	void FindPortalRender(uintptr_t shouldSkip)
	{
		const uint8_t* fn = reinterpret_cast<const uint8_t*>(shouldSkip);
		for (int i = 0; i < 0x80; ++i)
		{
			if (fn[i] == 0x8B && fn[i + 1] == 0x0D && fn[i + 6] == 0xE8)
			{
				g_PortalRenderPtr = *reinterpret_cast<const uintptr_t*>(fn + i + 2);
				const uint8_t* level = fn + i + 11 + *reinterpret_cast<const int32_t*>(fn + i + 7);
				if (level[0] == 0x8B && level[1] == 0x81 && level[6] == 0xC3)
					g_RecursionLevelOffset = *reinterpret_cast<const int32_t*>(level + 2);
				return;
			}
		}
	}
}

namespace Body
{
	void Install(Game* game, VR* vr)
	{
		g_Game_ = game;
		g_VR_ = vr;
		Offsets* o = game->m_Offsets;

		FindPortalRender(o->ShouldSkipPlayerInMainView.address);

		hkBuildTransformations.createHook((LPVOID)o->BuildTransformations.address, &dBuildTransformations);
		hkSetupBones.createHook((LPVOID)o->SetupBones.address, &dSetupBones);
		hkDrawModel.createHook((LPVOID)o->PortalPlayerDrawModel.address, &dDrawModel);
		hkShouldSkip.createHook((LPVOID)o->ShouldSkipPlayerInMainView.address, &dShouldSkip);

		hkBuildTransformations.enableHook();
		hkSetupBones.enableHook();
		hkDrawModel.enableHook();
		hkShouldSkip.enableHook();
	}

	void Publish(const Input& input)
	{
		std::lock_guard<std::mutex> lock(g_InputLock);
		g_Input = input;
	}

	void SetEnabled(bool enabled)
	{
		g_Enabled = enabled;
	}

	void SetHidden(bool hidden)
	{
		g_Hidden = hidden;
	}

	bool Visible()
	{
		return g_Enabled && !g_Hidden && g_VR_ && g_VR_->m_IsVREnabled && Netvars::Ready();
	}

	int ViewRecursionLevel()
	{
		if (!g_PortalRenderPtr)
			return 0;
		uintptr_t render = *reinterpret_cast<uintptr_t*>(g_PortalRenderPtr);
		if (!render)
			return 0;
		return *reinterpret_cast<int*>(render + g_RecursionLevelOffset);
	}
}
