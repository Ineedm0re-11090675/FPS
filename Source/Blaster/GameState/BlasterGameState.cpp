#include "BlasterGameState.h"

#include "Blaster/PlayerController/BlasterPlayerController.h"
#include "Blaster/PlayerState/BlasterPlayerState.h"
#include "Net/UnrealNetwork.h"

void ABlasterGameState::BlueTeamScore()
{
	++BlueTeamTotalScore;

	UWorld* World = GetWorld();
	ABlasterPlayerController* PlayerController = World ? Cast<ABlasterPlayerController>(World->GetFirstPlayerController()) : nullptr;
	if (PlayerController)
	{
		PlayerController->SetBlueTeamScore(BlueTeamTotalScore);
	}
}

void ABlasterGameState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ABlasterGameState, TopScoringPlayers);
	DOREPLIFETIME(ABlasterGameState,BlueTeamTotalScore);
	DOREPLIFETIME(ABlasterGameState,RedTeamTotalScore);
}

void ABlasterGameState::OnRep_BlueTeamScore()
{
	UWorld* World = GetWorld();
	ABlasterPlayerController* PlayerController = World ? Cast<ABlasterPlayerController>(World->GetFirstPlayerController()) : nullptr;
	if (PlayerController)
	{
		PlayerController->SetBlueTeamScore(BlueTeamTotalScore);
	}
}

void ABlasterGameState::OnRep_RedTeamScore()
{
	UWorld* World = GetWorld();
	ABlasterPlayerController* PlayerController = World ? Cast<ABlasterPlayerController>(World->GetFirstPlayerController()) : nullptr;
	if (PlayerController)
	{
		PlayerController->SetRedTeamScore(RedTeamTotalScore);
	}
}

void ABlasterGameState::RedTeamScore()
{
	++RedTeamTotalScore;

	UWorld* World = GetWorld();
	ABlasterPlayerController* PlayerController = World ? Cast<ABlasterPlayerController>(World->GetFirstPlayerController()) : nullptr;
	if (PlayerController)
	{
		PlayerController->SetRedTeamScore(RedTeamTotalScore);
	}
}

void ABlasterGameState::UpdateTopScore(class ABlasterPlayerState* ScoringPlayer)
{
	if (ScoringPlayer == nullptr) return;

	if (TopScoringPlayers.Num() == 0)
	{
		TopScoringPlayers.Add(ScoringPlayer);
		TopScore=ScoringPlayer->GetScore();
	}
	else if (ScoringPlayer->GetScore() == TopScore)
	{
		TopScoringPlayers.AddUnique(ScoringPlayer);
	}else if (ScoringPlayer->GetScore() > TopScore)
	{
		TopScoringPlayers.Empty();
		TopScoringPlayers.AddUnique(ScoringPlayer);
		TopScore=ScoringPlayer->GetScore();
	}
}
