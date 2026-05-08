#pragma once

#include "CoreMinimal.h"

FORCEINLINE bool SwuiCVarBool(int32 Override, bool DefaultValue)
{
	return Override < 0 ? DefaultValue : Override != 0;
}

FORCEINLINE int32 SwuiCVarInt(int32 Override, int32 DefaultValue)
{
	return Override < 0 ? DefaultValue : Override;
}

FORCEINLINE float SwuiCVarFloat(float Override, float DefaultValue)
{
	return Override < 0.f ? DefaultValue : Override;
}
