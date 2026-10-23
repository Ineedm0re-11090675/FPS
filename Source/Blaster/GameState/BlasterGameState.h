#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameState.h"
#include "BlasterGameState.generated.h"

UCLASS()
class BLASTER_API ABlasterGameState : public AGameState
{
	GENERATED_BODY()

public:
	void UpdateTopScore(class ABlasterPlayerState* ScoringPlayer);
	void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	UPROPERTY(Replicated)
	TArray<ABlasterPlayerState*> TopScoringPlayers;

	UPROPERTY()
	TArray<ABlasterPlayerState*> BlueTeamPlayers;
	UPROPERTY()
	TArray<ABlasterPlayerState*> RedTeamPlayers;

	UPROPERTY(ReplicatedUsing=OnRep_RedTeamScore)
	float RedTeamTotalScore = 0.f;
	UFUNCTION()
	void OnRep_RedTeamScore();

	UPROPERTY(ReplicatedUsing=OnRep_BlueTeamScore)
	float BlueTeamTotalScore = 0.f;
	UFUNCTION()
	void OnRep_BlueTeamScore();

	void RedTeamScore();

	void BlueTeamScore();
private:
	float TopScore = 0.f;	
};
