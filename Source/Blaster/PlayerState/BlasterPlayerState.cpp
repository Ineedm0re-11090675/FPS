#include "BlasterPlayerState.h"

#include "Blaster/Character/MyBlasterCharacter.h"
#include "Blaster/PlayerController/BlasterPlayerController.h"
#include "Net/UnrealNetwork.h"

void ABlasterPlayerState::AddToDefeat(int32 DefeatsAmount)
{
	Defeats+= DefeatsAmount;
	Character = Cast<AMyBlasterCharacter>(GetPawn());
	if (Character)
	{
		Controller= Cast<ABlasterPlayerController>(Character->GetController());
		if (Controller)
		{
			Controller->SetDefeat(Defeats);
		}
	}
}

void ABlasterPlayerState::AddToScore(float ScoreAmount)
{
	SetScore(GetScore() + ScoreAmount);
	Character = Cast<AMyBlasterCharacter>(GetPawn());
	if (Character)
	{
		Controller= Cast<ABlasterPlayerController>(Character->GetController());
		if (Controller)
		{
			Controller->SetScore(GetScore());
		}
	}
}

void ABlasterPlayerState::GetLifetimeReplicatedProps(TArray<class FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ABlasterPlayerState, Defeats);
	DOREPLIFETIME(ABlasterPlayerState, GrenadeAmount);
	DOREPLIFETIME(ABlasterPlayerState, CurrentTeam);
}

void ABlasterPlayerState::OnRep_Defeats()
{
	Character = Cast<AMyBlasterCharacter>(GetPawn());
	if (Character)
	{
		Controller= Cast<ABlasterPlayerController>(Character->GetController());
		if (Controller)
		{
			Controller->SetDefeat(Defeats);
		}
	}
}

void ABlasterPlayerState::OnRep_Score()
{
	Super::OnRep_Score();
	Character = Cast<AMyBlasterCharacter>(GetPawn());
	if (Character)
	{
		Controller= Cast<ABlasterPlayerController>(Character->GetController());
		if (Controller)
		{
			Controller->SetScore(GetScore());
		}
	}
}

void ABlasterPlayerState::OnRep_Team()
{
	AMyBlasterCharacter* Character = Cast<AMyBlasterCharacter>(GetPawn());
	if (Character)
	{
		Character->SetTeamColor(CurrentTeam);
	}
}

void ABlasterPlayerState::SetCurrentTeam(ETeam Team)
{
	CurrentTeam = Team;

	AMyBlasterCharacter* Character = Cast<AMyBlasterCharacter>(GetPawn());
	if (Character)
	{
		Character->SetTeamColor(CurrentTeam);
	}
}

void ABlasterPlayerState::SetGrenadeAmount(int32 Amount)
{
	GrenadeAmount = FMath::Max(Amount, 0);
}
