// Fill out your copyright notice in the Description page of Project Settings.


#include "BlasterGameMode.h"
#include "Blaster/PlayerController/BlasterPlayerController.h"
#include "Blaster/Character/MyBlasterCharacter.h"
#include "Blaster/GameState/BlasterGameState.h"
#include "Blaster/PlayerState/BlasterPlayerState.h"
#include "GameFramework/PlayerStart.h"
#include "Kismet/GameplayStatics.h"
#include "Blaster/BlasterComponent/CombatComponent.h"

namespace MatchState
{
	const FName Cooldown = FName("Cooldown");
}

ABlasterGameMode::ABlasterGameMode()
{
	//直到得到Start才会开始inprocess玩家可以在此期间飞行
	bDelayedStart=true;
}

void ABlasterGameMode::BeginPlay()
{
	Super::BeginPlay();
	if (UWorld* World = GetWorld())
	{
		LevelStartTime = World->GetTimeSeconds();
	}
}

float ABlasterGameMode::CalculateDamage(float Damage, AController*, AController*)
{
	return Damage;
}

void ABlasterGameMode::OnMatchStateSet()
{
	Super::OnMatchStateSet();

	UWorld* World = GetWorld();
	if (World == nullptr) return;

	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		ABlasterPlayerController* BlasterPlayer = Cast<ABlasterPlayerController>(*It);
		if (BlasterPlayer)
		{
			BlasterPlayer->OnSetMatchState(MatchState,bTeamsMatch);
			if (MatchState == MatchState::Cooldown)
			{
				AMyBlasterCharacter* BlasterCharacter =
					Cast<AMyBlasterCharacter>(BlasterPlayer->GetPawn());

				if (BlasterCharacter)
				{
					BlasterCharacter->bDisableGamePlay = true;

					if (BlasterCharacter->GetCombat())
					{
						BlasterCharacter->GetCombat()->FireButtonPressed(false);
					}
				}
			}
		}
	}
}

void ABlasterGameMode::PlayerEliminated(class AMyBlasterCharacter* ElimmedCharacter,
                                        class ABlasterPlayerController* VictimController, ABlasterPlayerController* AttackerController)
{
	ABlasterPlayerState* AttackerPlayerState = AttackerController?Cast<ABlasterPlayerState>(AttackerController->PlayerState):nullptr;
	ABlasterPlayerState* VictimPlayerState = VictimController?Cast<ABlasterPlayerState>(VictimController->PlayerState):nullptr;

	ABlasterGameState* BlasterGameState = GetGameState<ABlasterGameState>();
	if (AttackerPlayerState && VictimPlayerState && VictimPlayerState != AttackerPlayerState && BlasterGameState)
	{
		TArray<ABlasterPlayerState*> PlayerCurrentlyInLead;
		for (auto Leader : BlasterGameState->TopScoringPlayers)
		{
			PlayerCurrentlyInLead.Add(Leader);
		}
		//排除自杀 
		AttackerPlayerState->AddToScore(1.f);
		BlasterGameState->UpdateTopScore(AttackerPlayerState);

		if (BlasterGameState->TopScoringPlayers.Contains(AttackerPlayerState))
		{
			AMyBlasterCharacter* Leader =Cast<AMyBlasterCharacter>(AttackerPlayerState->GetPawn());
			if (Leader)
			{
				Leader->MulticastGainLeader();
			}
		}
		for (int32 i = 0; i < PlayerCurrentlyInLead.Num(); i++)
		{
			if (!BlasterGameState->TopScoringPlayers.Contains(PlayerCurrentlyInLead[i]))
			{
				AMyBlasterCharacter* Loser =Cast<AMyBlasterCharacter>(PlayerCurrentlyInLead[i]->GetPawn());
				if (Loser)
				{
					Loser->MulticastLostLeader(); 
				}
			}
		}
	}
	if (VictimPlayerState)
	{
		VictimPlayerState->AddToDefeat(1);
	}
	if (ElimmedCharacter)
	{
		ElimmedCharacter->Elim(false);
	}

	UWorld* World = GetWorld();
	if (World == nullptr) return;

	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		ABlasterPlayerController* Controller = Cast<ABlasterPlayerController>(*It);
		if (Controller && VictimPlayerState && AttackerPlayerState)
		{
			Controller->BroadCastElim(AttackerPlayerState, VictimPlayerState);
		}
	}
}

void ABlasterGameMode::PlayerLeaveGame(ABlasterPlayerState* PlayerState)
{
	//Call Elim ,Pass in true for bLeaveGame
	if (PlayerState == nullptr) return;
	ABlasterGameState* BlasterGameState = GetGameState<ABlasterGameState>();
	if (BlasterGameState && BlasterGameState->TopScoringPlayers.Contains(PlayerState))
	{
		BlasterGameState->TopScoringPlayers.Remove(PlayerState);
	}
	AMyBlasterCharacter* LeavingCharacter = Cast<AMyBlasterCharacter>(PlayerState->GetPawn());
	if (LeavingCharacter)
	{
		LeavingCharacter->Elim(true);
	}
}

void ABlasterGameMode::RequestRespawn(ACharacter* ElimmedCharacter, AController* ElimmedController)
{
	FVector DeathLocation = FVector::ZeroVector;

	if (ElimmedCharacter)
	{
		// 先记录死亡位置，后面角色 Destroy 了就不能安全用了
		//Reset抽离Controller
		DeathLocation = ElimmedCharacter->GetActorLocation();

		ElimmedCharacter->Reset();
		ElimmedCharacter->Destroy();
	}

	if (ElimmedController)
	{
		//找所以的Restart点，计算最远 
		TArray<AActor*> PlayerStarts;
		UGameplayStatics::GetAllActorsOfClass(
			this,
			APlayerStart::StaticClass(),
			PlayerStarts
		);

		if (PlayerStarts.Num() == 0)
		{
			return;
		}

		AActor* BestPlayerStart = PlayerStarts[0];
		float MaxDistanceSquared = FVector::DistSquared(
			DeathLocation,
			BestPlayerStart->GetActorLocation()
		);

		for (AActor* PlayerStart : PlayerStarts)
		{
			if (PlayerStart == nullptr) continue;

			const float DistanceSquared = FVector::DistSquared(
				DeathLocation,
				PlayerStart->GetActorLocation()
			);

			if (DistanceSquared > MaxDistanceSquared)
			{
				MaxDistanceSquared = DistanceSquared;
				BestPlayerStart = PlayerStart;
			}
		}

		RestartPlayerAtPlayerStart(ElimmedController, BestPlayerStart);
	}
}

void ABlasterGameMode::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	UWorld* World = GetWorld();
	if (World == nullptr) return;

	const float CurrentTime = World->GetTimeSeconds();
	if (MatchState == MatchState::WaitingToStart)
	{
		CountDownTime = WarmUpTime -CurrentTime+LevelStartTime;
		if (CountDownTime <=0.f)
		{
			StartMatch();
		}
	}else if (MatchState == MatchState::InProgress)
	{
		CountDownTime = MatchTime+WarmUpTime -CurrentTime+LevelStartTime;
		if (CountDownTime <=0.f)
		{
			SetMatchState(MatchState::Cooldown);
		}
	}else if (MatchState == MatchState::Cooldown)
	{
		CountDownTime =  CooldownTime+MatchTime+WarmUpTime -CurrentTime+LevelStartTime;
		if (CountDownTime <=0.f)
		{
			RestartGame();
		}
	}
}
