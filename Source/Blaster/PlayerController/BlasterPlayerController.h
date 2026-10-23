// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "BlasterPlayerController.generated.h"


DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FHighPingDelegate,bool,bHighPing);
/**
 * 
 */
UCLASS()
class BLASTER_API ABlasterPlayerController : public APlayerController
{
	GENERATED_BODY()
protected:
	virtual void SetupInputComponent() override;
	virtual void BeginPlay() override;
	virtual void OnPossess(APawn* InPawn) override;
	virtual void Tick(float DeltaTime) override;
	void SetHudTime();
	/*
	 *Sync Time between client and server
	 */
	UFUNCTION(Server, Reliable)
	//查询当前服务器时间，纯如当前client时间
	void ServerRequestServerTime(float TimeOfClientRequest);

	//汇报当前的Server时间到client
	UFUNCTION(Client, Reliable)
	void ClientReportServerTime(float TimeOfClientRequest,float TimeServerReceiveClientRequest);
	float ClientServerDelta =0.f;//Server Client DeltaTime

	void PollInit();

	virtual void GetLifetimeReplicatedProps(TArray<class FLifetimeProperty>& OutLifetimeProps) const override;
public:
	void SetHealth(float Health,float MaxHealth);
	void SetShield(float Shield,float MaxShield);
	void SetScore(float Score);
	void SetDefeat(int32 Defeats);
	void SetAmmo(int32 Ammo);
	void SetCarriedAmmo(int32 Ammo);
	void SetHUDMatchCountDown(float time);
	void SetHUDWarmUpCountDown(float time);
	void SetWeaponAmmoVisible(bool bVisible);
	void SetHUDGrenade(int32 GrenadeAmount);
	virtual float GetServerTime();
	virtual void ReceivedPlayer() override;
	void CheckTimeSync(float Delta);
	void OnSetMatchState(FName State , bool bTeamMatch = false);

	void HandleMatchHasStarted(bool bTeamMatch = false);
	void HandleCoolDown();
	void CheckPing(float DeltaTime);
	UFUNCTION(Server, Reliable)
	void ServerCheckMatchState();
	UFUNCTION(Client, Reliable)
	void ClientJoinMidGame(FName State,float WarmUp,float Match,float Starting,float Cooldown,bool bTeamMatch); // Codex: Carry team match mode through the join-mid-game sync path.

	void HighPingWarning();
	void StopHighPingWarning();

	float SingleTripTime = 0.f;

	FHighPingDelegate HighPingDelegate;

	void ShowReturnToMainMenu();

	void BroadCastElim(APlayerState* Attacker,APlayerState* Victim);
	
	UFUNCTION(Client, Reliable)
	void ClientBroadCastElim(APlayerState* Attacker,APlayerState* Victim);

	void InitTeamScore();
	
	void HideTeamScore();

	void SetRedTeamScore(int32 TeamScore);

	void SetBlueTeamScore(int32 TeamScore);

	UPROPERTY(ReplicatedUsing=OnRep_ShowTeamScore)
	bool bShowTeamScore=false;
	
	UFUNCTION()
	void OnRep_ShowTeamScore();

	FString GetInfoText(const TArray<class ABlasterPlayerState*>& Player);
	FString GetTeamInfoText(class ABlasterGameState* GameState);
private:
	void RequestServerTimeSync();

	UPROPERTY()
	class ABlasterHUD* BlasterHUD;
	UPROPERTY()
	class ABlasterGameMode* BlasterGameMode;
	
	float MatchTime=0.f;
	float WarmUpTime=0.f;
	float CooldownTime=0.f;
	float LevelStartTime=0.f;
	uint32 CountDownInt=0;
	UPROPERTY(EditAnywhere,Category="Time")
	float TimeSyncFrequency = 5.f;
	float TimeSyncStart=0.f;
	bool bHasSyncedServerTime=false;

	UPROPERTY(ReplicatedUsing=OnRep_MatchState)
	FName MatchState;

	UFUNCTION()
	void OnRep_MatchState();

	UPROPERTY()
	class UCharacterOverlay* CharacterOverlay;

	float HUDHealth;
	float HudMaxHealth;
	bool bInitHealth=false;
	float HUDShield;
	float HudMaxShield;
	bool bInitShield=false;
	float HUDScore;
	float HUDMaxScore;
	bool bInitScore=false;
	int32 HUDDefeats;
	int32 HUDMaxDefeats;
	bool bInitDefeats=false;
	int32 HUDGrenade;
	bool bInitGrenade=false;
	float HUDCarriedAmmo;
	bool bInitCarriedAmmo=false;
	float HUDWeaponAmmo;
	bool bInitWeaponAmmo =false;

	UPROPERTY()
	class AMyBlasterCharacter* HUDInitializedCharacter = nullptr;
	float HighPingRunningTime=0.f;
	bool bHighPingWarningActive = false;
	UPROPERTY(EditAnywhere)
	float CheckPingFrequency = 5.f;

	UFUNCTION(Server,Reliable)
	void ServerReportPingState(bool bHighPing);

	UPROPERTY(EditAnywhere)
	float HighPingThreshold = 50.f;

	/*
	 *Menu Widget
	 */
	UPROPERTY(EditAnywhere,Category= HUD)
	TSubclassOf<class UUserWidget> ReturnToMainMenuWidget;

	UPROPERTY()
	class UReturnToMainMenu* ReturnToMainMenu;

	bool bReturnToMainMenuOpen =false;
};
