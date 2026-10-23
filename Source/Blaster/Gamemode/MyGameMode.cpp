// Fill out your copyright notice in the Description page of Project Settings.


#include "MyGameMode.h"
#include "GameFramework/GameState.h"

void AMyGameMode::PostLogin(APlayerController* PlayerController)
{
	Super::PostLogin(PlayerController);
	if (GameState == nullptr)
	{
		return;
	}
	const int32 NumOfPlayers = GameState.Get()->PlayerArray.Num();
	if (NumOfPlayers >= 2)
	{
		UWorld* World = GetWorld();
		if (World)
		{
			bUseSeamlessTravel = true;
			World->ServerTravel(TEXT("/Game/maps/BlasterMap?listen"));
		}
	}
}
