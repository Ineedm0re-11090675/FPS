#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "InteractWithCrossHairInterface.generated.h"



//
//给UE
//
UINTERFACE(MinimalAPI)
class UInteractWithCrossHairInterface : public UInterface
{
	GENERATED_BODY()
};

//
//给CPP多态
//

class BLASTER_API IInteractWithCrossHairInterface
{
	GENERATED_BODY()
public:
	
};
