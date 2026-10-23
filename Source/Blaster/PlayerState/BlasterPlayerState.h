#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerState.h"
#include "Blaster/BlasterType/Team.h"
#include "BlasterPlayerState.generated.h"

UCLASS()
class BLASTER_API ABlasterPlayerState : public APlayerState
{
	GENERATED_BODY()
public:
	virtual void OnRep_Score() override;
	void AddToScore(float ScoreAmount);
	void AddToDefeat(int32 DefeatsAmount);
	void SetGrenadeAmount(int32 Amount);
	FORCEINLINE int32 GetGrenadeAmount() const { return GrenadeAmount; }
	/*
	 *Replication notifies
	 */
	UFUNCTION()
	virtual void OnRep_Defeats();
	virtual void GetLifetimeReplicatedProps(TArray<class FLifetimeProperty>& OutLifetimeProps) const override;
private:
	//Controller可以访问state，但是state不能反向访问
	UPROPERTY()
	class AMyBlasterCharacter* Character;
	UPROPERTY()
	class ABlasterPlayerController* Controller;
	
	UPROPERTY(ReplicatedUsing=OnRep_Defeats)
	int32 Defeats;

	UPROPERTY(Replicated)
	int32 GrenadeAmount = 4;

	UPROPERTY(ReplicatedUsing = OnRep_Team)
	ETeam CurrentTeam = ETeam::ET_NoTeam;
	
	UFUNCTION()
	void OnRep_Team();
public:
	FORCEINLINE ETeam GetCurrentTeam() const { return CurrentTeam; }
	void SetCurrentTeam(ETeam Team);
};
