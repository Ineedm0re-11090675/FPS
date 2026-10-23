#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "ReturnToMainMenu.generated.h"

UCLASS()
class BLASTER_API UReturnToMainMenu : public UUserWidget
{
	GENERATED_BODY()

public:
	void SetUpMenu();
	void TearDownMenu();
protected:
	virtual bool Initialize() override;	
	
	UFUNCTION()
	void OnDestroySession(bool bWasSuccess); 

	UFUNCTION()
	void OnPlayerLeftGame();
	
private:
	UPROPERTY(meta = (BindWidget) )
	class UButton* ReturnButton;	
	
	UFUNCTION()
	void ReturnButtonClicked();

	void ReturnToFrontEnd();

	UPROPERTY()
	class APlayerController* PlayerController;

	UPROPERTY()
	class UMultiPlayerSessionsSubSystem* MultiPlayerSessionsSubSystem;
};
