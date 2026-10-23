#pragma once

#include "CoreMinimal.h"
#include "Blaster/BlasterType/Team.h"
#include "Blaster/Gamemode/BlasterGameMode.h"
#include "TeamGamemode.generated.h"

class ABlasterPlayerState;
class APlayerStart;
class ATeamPlayerStart;

UCLASS()
class BLASTER_API ATeamGameMode : public ABlasterGameMode
{
	GENERATED_BODY()
public:
	ATeamGameMode();
	virtual AActor* ChoosePlayerStart_Implementation(AController* Player) override;
	virtual void PostLogin(APlayerController* PlayerController) override;
	virtual void Logout(AController* Exiting) override;
	virtual float CalculateDamage(float Damage,AController* Attacker , AController* Victim) override;
	virtual void PlayerEliminated(class AMyBlasterCharacter* ElimmedCharacter,class ABlasterPlayerController* VictimController,ABlasterPlayerController* AttackerController) override;
	virtual void RequestRespawn(ACharacter* ElimmedCharacter,AController* ElimmedController) override;
protected:
	virtual void HandleMatchHasStarted() override;
	virtual bool ShouldSpawnAtStartSpot(AController* Player) override;

private:
	void AssignTeamIfNeeded(ABlasterPlayerState* PlayerState);
	APlayerStart* ChooseTeamPlayerStart(AController* Player, ETeam Team, bool& bFoundAnyTeamPlayerStart);
};
