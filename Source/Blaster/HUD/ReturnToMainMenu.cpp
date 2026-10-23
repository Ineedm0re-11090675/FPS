#include "ReturnToMainMenu.h"

#include "MultiPlayerSessionsSubSystem.h"
#include "Components/Button.h"
#include "Blaster/Character/MyBlasterCharacter.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerController.h"

bool UReturnToMainMenu::Initialize()
{
	if (!Super::Initialize())
	{
		return false;
	}
	
	return true;
}

void UReturnToMainMenu::OnDestroySession(bool)
{
	ReturnToFrontEnd();
}

void UReturnToMainMenu::OnPlayerLeftGame()
{
	if (MultiPlayerSessionsSubSystem)
	{
		MultiPlayerSessionsSubSystem->DestroySession();
		return;
	}

	ReturnToFrontEnd();
}

void UReturnToMainMenu::ReturnButtonClicked()
{
	if (ReturnButton == nullptr)
	{
		return;
	}

	ReturnButton->SetIsEnabled(false);
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		ReturnButton->SetIsEnabled(true);
		return;
	}

	PlayerController = PlayerController == nullptr ? World->GetFirstPlayerController() : PlayerController;
	if (PlayerController == nullptr)
	{
		ReturnButton->SetIsEnabled(true);
		return;
	}

	AMyBlasterCharacter* BlasterCharacter = Cast<AMyBlasterCharacter>(PlayerController->GetPawn());
	if (BlasterCharacter == nullptr)
	{
		OnPlayerLeftGame();
		return;
	}

	BlasterCharacter->OnLeftGame.AddUniqueDynamic(this, &UReturnToMainMenu::OnPlayerLeftGame);
	BlasterCharacter->ServerLeaveGame();
}

void UReturnToMainMenu::ReturnToFrontEnd()
{
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	TearDownMenu();

	if (AGameModeBase* GameMode = World->GetAuthGameMode<AGameModeBase>())
	{
		GameMode->ReturnToMainMenuHost();
		return;
	}

	PlayerController = PlayerController == nullptr ? World->GetFirstPlayerController() : PlayerController;
	if (PlayerController)
	{
		PlayerController->ClientTravel(TEXT("/Game/maps/GameStartUpLevel"), TRAVEL_Absolute);
	}
}

void UReturnToMainMenu::SetUpMenu()
{
	AddToViewport();
	SetVisibility(ESlateVisibility::Visible);
	SetIsFocusable(true);

	UWorld* world = GetWorld();
	if (world)
	{
		PlayerController = PlayerController == nullptr ?  world->GetFirstPlayerController() : PlayerController; 
		if (PlayerController)
		{
			FInputModeGameAndUI PlayerInputMode;
			PlayerInputMode.SetWidgetToFocus(TakeWidget());
			PlayerController->SetInputMode(PlayerInputMode);
			PlayerController->SetShowMouseCursor(true);
		}
	}
	if (ReturnButton && !ReturnButton->OnClicked.IsBound())
	{
		ReturnButton->OnClicked.AddDynamic(this, &UReturnToMainMenu::ReturnButtonClicked);
	}
	UGameInstance* GameInstance = GetGameInstance();
	if(GameInstance)
	{
		MultiPlayerSessionsSubSystem = GameInstance->GetSubsystem<UMultiPlayerSessionsSubSystem>();
		if (MultiPlayerSessionsSubSystem)
		{
			MultiPlayerSessionsSubSystem->MultiplayerOnDestroySessionComplete.AddDynamic(this,&UReturnToMainMenu::OnDestroySession);
		}
	}
}

void UReturnToMainMenu::TearDownMenu()
{
	RemoveFromParent();
	UWorld* world = GetWorld();
	if (world)
	{
		PlayerController = PlayerController == nullptr ?  world->GetFirstPlayerController() : PlayerController;
		if (PlayerController)
		{
			FInputModeGameOnly PlayerInputMode;
			PlayerController->SetInputMode(PlayerInputMode);
			PlayerController->SetShowMouseCursor(false);
		}
	}
	if (ReturnButton && ReturnButton->OnClicked.IsBound())
	{
		ReturnButton->OnClicked.RemoveDynamic (this, &UReturnToMainMenu::ReturnButtonClicked);
	}
	if (MultiPlayerSessionsSubSystem)
	{
		MultiPlayerSessionsSubSystem->MultiplayerOnDestroySessionComplete.RemoveDynamic(this,&UReturnToMainMenu::OnDestroySession);
	}
}
