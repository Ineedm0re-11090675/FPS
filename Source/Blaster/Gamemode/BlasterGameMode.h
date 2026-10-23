// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameMode.h"
#include "BlasterGameMode.generated.h"


namespace  MatchState
{
	//extern，在别处定义的意思
	//BLASTER_API可以在DLL库外部使用
	extern BLASTER_API const FName Cooldown; //游戏时长已达到，展示胜者然后cooldown
}
/**
 * 
 */
UCLASS()
class BLASTER_API ABlasterGameMode : public AGameMode
{
	GENERATED_BODY()
public:
	ABlasterGameMode();
	virtual void PlayerEliminated(class AMyBlasterCharacter* ElimmedCharacter,class ABlasterPlayerController* VictimController,ABlasterPlayerController* AttackerController);
	virtual void RequestRespawn(ACharacter* ElimmedCharacter,AController* ElimmedController);
	UPROPERTY(EditDefaultsOnly)
	float WarmUpTime=10.f;
	UPROPERTY(EditDefaultsOnly)
	float MatchTime=120.f;
	UPROPERTY(EditDefaultsOnly)
	float CooldownTime=10.f;
	float LevelStartTime=0.f;

	void PlayerLeaveGame(class ABlasterPlayerState* PlayerState);

	virtual float CalculateDamage(float Damage,AController* Attacker , AController* Victim);

	bool bTeamsMatch =false;
protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;
	virtual void OnMatchStateSet() override;
private:
	float CountDownTime;
public:
	FORCEINLINE float GetCountDownTime() const {return CountDownTime;} ;
};
