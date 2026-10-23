#pragma once

#include "CoreMinimal.h"
#include "Blaster/BlasterType/Team.h"
#include "GameFramework/PlayerStart.h"
#include "TeamPlayerStart.generated.h"

UCLASS(Blueprintable)
class BLASTER_API ATeamPlayerStart : public APlayerStart
{
	GENERATED_BODY()

public:
	// Codex: lets each placed PlayerStart declare which team is allowed to spawn there.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Team")
	ETeam Team = ETeam::ET_NoTeam;
};
