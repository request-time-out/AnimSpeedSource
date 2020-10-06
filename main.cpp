#include "Windows.h"
#include "../skse/PluginAPI.h"
#include "../skse/skse_version.h"

// Include to register papyrus command.
#include "../skse/PapyrusVM.h"
#include "../skse/PapyrusNativeFunctions.h"
#include "../skse/GameReferences.h"

#include <vector>
#include <mutex>

#define ANIMPLUGIN_VERSION 1

// Helper asm stuff.
#define START_ASM1(b) void * jumpAddress = NULL; _asm { mov jumpAddress, offset b##CodeStart
#define START_ASM2(b) jmp b##CodeEnd
#define START_ASM3(b) b##CodeStart:

#define END_ASM(b, addr1, addr2) b##CodeEnd: \
} \
	WriteJump(addr1, addr2, (int)jumpAddress);

// End ASM.

// Helper function to write detour.
bool WriteJump(int addressFrom1, int addressFrom2, int addressTo)
{
	DWORD oldProtect = 0;

	int len1 = addressFrom2 - addressFrom1;
	if (VirtualProtect((void *)addressFrom1, len1, PAGE_EXECUTE_READWRITE, &oldProtect))
	{
		*((unsigned char *)addressFrom1) = (unsigned char)0xE9;
		*((int *)(addressFrom1 + 1)) = (int)addressTo - addressFrom1 - 5;
		for (int i = 5; i < len1; i++)
			*((unsigned char *)(i + addressFrom1)) = (unsigned char)0x90;
		if (VirtualProtect((void *)addressFrom1, len1, oldProtect, &oldProtect))
			return true;
	}

	return false;
}

struct TimeData
{
	int FormId;
	float TargetTime;
	float CurrentTime;
	float Transition;
	int AnimPtr;

	void Update(float time)
	{
		if (TargetTime == CurrentTime)
			return;
		if (Transition <= 0.0f || Transition <= time)
		{
			CurrentTime = TargetTime;
			Transition = 0.0f;
			return;
		}
		if (time <= 0.0f)
			return;

		float ratio = time / Transition;
		float diff = TargetTime - CurrentTime;
		diff *= ratio;
		CurrentTime += diff;
		Transition -= time;
	}

	bool ShouldRemove()
	{
		return TargetTime == CurrentTime && CurrentTime == 1.0f;
	}
};

typedef std::vector<TimeData*> TimeList;
typedef std::vector<int> AnimList;
typedef TESForm * (*_LookupFormByID)(UInt32 id);

// Random place in frame update.
int frameUpdate1 = 0x69CC5A;
int frameUpdate2 = 0x69CC60;

// Animation graph delta hook.
int animDelta1 = 0x64F844;
int animDelta2 = 0x64F84A;

// Our internal data.
TimeList timeInfo;
std::recursive_mutex mtx;

// Our plugin handle.
PluginHandle pluginHandle = kPluginHandle_Invalid;

float GetOurTime(int animPtr)
{
	float curTime = 1.0f;
	if (animPtr == 0)
		return curTime;

	mtx.lock();
	for (TimeList::iterator itr = timeInfo.begin(); itr != timeInfo.end(); itr++)
	{
		if ((*itr)->AnimPtr == animPtr)
		{
			curTime = (*itr)->CurrentTime;
			break;
		}
	}
	mtx.unlock();

	return curTime;
}

void SetOurTime(int actorPtr, float target, float transition, bool absolute)
{
	if (actorPtr == 0)
		return;

	int formId = *((int*)(actorPtr + 0xC));
	TimeData * data = NULL;

	mtx.lock();
	for (TimeList::iterator itr = timeInfo.begin(); itr != timeInfo.end(); itr++)
	{
		if ((*itr)->FormId == formId)
		{
			data = *itr;
			break;
		}
	}

	if (data == NULL)
	{
		data = new TimeData();
		data->FormId = formId;
		data->CurrentTime = 1.0f;
		data->TargetTime = 1.0f;
		data->Transition = 0.0f;
		data->AnimPtr = 0;

		timeInfo.push_back(data);
	}

	if (absolute)
	{
		data->TargetTime = target;
		data->Transition = transition;
	}
	else
	{
		float diff = data->CurrentTime - target;
		if (diff < 0.0f)
			diff = -diff;

		data->TargetTime = target;
		data->Transition = diff * transition;
	}
	mtx.unlock();
}

void UpdateTimes()
{
	float diff = *((float*)0x1B4ADE4);
	if (diff < 0.0f)
		diff = 0.0f;

	static _LookupFormByID func = (_LookupFormByID)0x451A30;

	mtx.lock();
	for (TimeList::iterator itr = timeInfo.begin(); itr != timeInfo.end();)
	{
		(*itr)->Update(diff);
		if ((*itr)->ShouldRemove())
		{
			itr = timeInfo.erase(itr);
			continue;
		}

		int actor = (int)func((*itr)->FormId);
		if (actor == 0)
			(*itr)->AnimPtr = 0;
		else
			(*itr)->AnimPtr = actor + 0x20;

		itr++;
	}
	mtx.unlock();
}

void UpdateTimes2(int animGraphPtr, int updateData)
{
	float delta = *((float*)updateData);
	if (delta <= 0.0f)
		return;

	delta *= GetOurTime(animGraphPtr);

	*((float*)updateData) = delta;
}

void i_SetAnimationSpeed(StaticFunctionTag * base, Actor * actor, float scale, float transition, UInt32 absolute)
{
	SetOurTime((int)actor, scale, transition, absolute != 0);
}

void i_ResetTransition(StaticFunctionTag * base, Actor * actor)
{
	mtx.lock();
	for (TimeList::iterator itr = timeInfo.begin(); itr != timeInfo.end(); itr++)
	{
		if (actor != NULL && (*itr)->FormId != actor->formID)
			continue;

		(*itr)->TargetTime = (*itr)->CurrentTime;
		(*itr)->Transition = 0.0f;
	}
	mtx.unlock();
}

void i_ResetAll(StaticFunctionTag * base)
{
	mtx.lock();
	timeInfo.clear();
	mtx.unlock();
}

UInt32 i_GetVersion(StaticFunctionTag * base)
{
	return ANIMPLUGIN_VERSION;
}

// Register command to give to SKSE.
bool _registerAnimationFunctions(VMClassRegistry * registry)
{
	const char * className = "AnimSpeedHelper";

	{
		const char * methodName = "SetAnimationSpeed";
		registry->RegisterFunction(new NativeFunction4<StaticFunctionTag, void, Actor*, float, float, UInt32>(methodName, className, i_SetAnimationSpeed, registry));
		registry->SetFunctionFlags(className, methodName, VMClassRegistry::kFunctionFlag_NoWait);
	}

	{
		const char * methodName = "ResetTransition";
		registry->RegisterFunction(new NativeFunction1<StaticFunctionTag, void, Actor*>(methodName, className, i_ResetTransition, registry));
		registry->SetFunctionFlags(className, methodName, VMClassRegistry::kFunctionFlag_NoWait);
	}

	{
		const char * methodName = "ResetAll";
		registry->RegisterFunction(new NativeFunction0<StaticFunctionTag, void>(methodName, className, i_ResetAll, registry));
		registry->SetFunctionFlags(className, methodName, VMClassRegistry::kFunctionFlag_NoWait);
	}

	{
		const char * methodName = "GetVersion";
		registry->RegisterFunction(new NativeFunction0<StaticFunctionTag, UInt32>(methodName, className, i_GetVersion, registry));
		registry->SetFunctionFlags(className, methodName, VMClassRegistry::kFunctionFlag_NoWait);
	}

	return true;
}

void _handleSKSEMessage(SKSEMessagingInterface::Message * msg)
{
	if (!msg)
		return;

	switch (msg->type)
	{
	case SKSEMessagingInterface::kMessage_NewGame:
	case SKSEMessagingInterface::kMessage_PostLoadGame:
		i_ResetAll(NULL);
		break;
	}
}

extern "C"
{
	bool SKSEPlugin_Query(const SKSEInterface * skse, PluginInfo * info)
	{
		info->infoVersion = PluginInfo::kInfoVersion;
		info->name = "animspeed plugin";
		info->version = ANIMPLUGIN_VERSION;
		pluginHandle = skse->GetPluginHandle();

		if (skse->isEditor || skse->runtimeVersion != RUNTIME_VERSION_1_9_32_0)
			return false;
		return true;
	}

	bool SKSEPlugin_Load(const SKSEInterface * skse)
	{
		// Hook frame update. So we can update transition times.
		{
			START_ASM1(FU1)
			START_ASM2(FU1)
			START_ASM3(FU1)

			mov edi, 1
			push edi

			pushad
			pushfd
			call UpdateTimes
			popfd
			popad

			jmp frameUpdate2
			
				   END_ASM(FU1, frameUpdate1, frameUpdate2)
		};

		// Hook anim graph delta time.
		{
			START_ASM1(AG1)
				START_ASM2(AG1)
				START_ASM3(AG1)

				mov edi, [esp+0x14]
				xor bl, bl

				pushad
				pushfd

				push edi
				push ecx
				call UpdateTimes2
				add esp, 8

				popfd
				popad

				jmp animDelta2

				END_ASM(AG1, animDelta1, animDelta2)
		};

		// Register papyrus command.
		{
			SKSEPapyrusInterface * papyrusInterface = (SKSEPapyrusInterface *)skse->QueryInterface(kInterface_Papyrus);
			if (papyrusInterface)
				papyrusInterface->Register(_registerAnimationFunctions);
		};

		{
			SKSEMessagingInterface * messageInterface = (SKSEMessagingInterface*)skse->QueryInterface(kInterface_Messaging);
			if (messageInterface)
				messageInterface->RegisterListener(pluginHandle, "SKSE", _handleSKSEMessage);
		};

		return true;
	}
};
