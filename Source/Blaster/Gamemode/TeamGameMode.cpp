#include "TeamGameMode.h"

#include "Blaster/GameState/BlasterGameState.h"
#include "Blaster/PlayerController/BlasterPlayerController.h"
#include "Blaster/Gamemode/TeamPlayerStart.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerStart.h"
#include "Engine/PlayerStartPIE.h"
#include "Kismet/GameplayStatics.h"
#include "Blaster/PlayerState/BlasterPlayerState.h"

ATeamGameMode::ATeamGameMode()
{
	bTeamsMatch = true;
}

float ATeamGameMode::CalculateDamage(float Damage, AController* Attacker, AController* Victim)
{
	if (Attacker == nullptr || Victim == nullptr) return Damage;
	if (Attacker == Victim) return Damage;

	ABlasterPlayerState* AttackerState = Attacker->GetPlayerState<ABlasterPlayerState>();
	ABlasterPlayerState* VictimState = Victim->GetPlayerState<ABlasterPlayerState>();
	if (VictimState == nullptr || AttackerState == nullptr) return Damage;
	if (VictimState->GetCurrentTeam() == AttackerState->GetCurrentTeam())
	{
		return 0.f;
	}
	return Damage;
}

void ATeamGameMode::AssignTeamIfNeeded(ABlasterPlayerState* PlayerState)
{
	// Codex: PlayerState owns team identity; keep GameState arrays as indexes, not the source of truth.
	ABlasterGameState* BlasterGameState = GetGameState<ABlasterGameState>();
	if (BlasterGameState == nullptr || PlayerState == nullptr) return;

	if (PlayerState->GetCurrentTeam() == ETeam::ET_RedTeam)
	{
		BlasterGameState->BlueTeamPlayers.Remove(PlayerState);
		BlasterGameState->RedTeamPlayers.AddUnique(PlayerState);
		return;
	}

	if (PlayerState->GetCurrentTeam() == ETeam::ET_BlueTeam)
	{
		BlasterGameState->RedTeamPlayers.Remove(PlayerState);
		BlasterGameState->BlueTeamPlayers.AddUnique(PlayerState);
		return;
	}

	if (BlasterGameState->BlueTeamPlayers.Num() >= BlasterGameState->RedTeamPlayers.Num())
	{
		BlasterGameState->BlueTeamPlayers.Remove(PlayerState);
		BlasterGameState->RedTeamPlayers.AddUnique(PlayerState);
		PlayerState->SetCurrentTeam(ETeam::ET_RedTeam);
	}
	else
	{
		BlasterGameState->RedTeamPlayers.Remove(PlayerState);
		BlasterGameState->BlueTeamPlayers.AddUnique(PlayerState);
		PlayerState->SetCurrentTeam(ETeam::ET_BlueTeam);
	}
}

APlayerStart* ATeamGameMode::ChooseTeamPlayerStart(AController* Player, ETeam Team, bool& bFoundAnyTeamPlayerStart)
{
	// Codex: mirror UE's unoccupied-first selection, but only inside the requested team pool.
	UWorld* World = GetWorld();
	bFoundAnyTeamPlayerStart = false;
	if (World == nullptr || Team == ETeam::ET_NoTeam) return nullptr;

	UClass* PawnClass = GetDefaultPawnClassForController(Player);
	APawn* PawnToFit = PawnClass ? PawnClass->GetDefaultObject<APawn>() : nullptr;

	TArray<APlayerStart*> UnoccupiedTeamStarts;
	TArray<APlayerStart*> OccupiedTeamStarts;
	TArray<APlayerStart*> UnoccupiedFallbackStarts;
	TArray<APlayerStart*> OccupiedFallbackStarts;

	for (TActorIterator<APlayerStart> It(World); It; ++It)
	{
		APlayerStart* PlayerStart = *It;
		if (PlayerStart == nullptr) continue;
		if (PlayerStart->IsA<APlayerStartPIE>()) return PlayerStart;

		const ATeamPlayerStart* TeamPlayerStart = Cast<ATeamPlayerStart>(PlayerStart);
		bFoundAnyTeamPlayerStart = bFoundAnyTeamPlayerStart || TeamPlayerStart != nullptr;

		const bool bMatchesTeam = TeamPlayerStart && TeamPlayerStart->Team == Team;
		const bool bFallbackStart = TeamPlayerStart == nullptr || TeamPlayerStart->Team == ETeam::ET_NoTeam;
		if (!bMatchesTeam && !bFallbackStart) continue;

		FVector StartLocation = PlayerStart->GetActorLocation();
		const FRotator StartRotation = PlayerStart->GetActorRotation();
		const bool bUnoccupied = PawnToFit == nullptr || !World->EncroachingBlockingGeometry(PawnToFit, StartLocation, StartRotation);

		if (bUnoccupied)
		{
			(bMatchesTeam ? UnoccupiedTeamStarts : UnoccupiedFallbackStarts).Add(PlayerStart);
		}
		else if (World->FindTeleportSpot(PawnToFit, StartLocation, StartRotation))
		{
			(bMatchesTeam ? OccupiedTeamStarts : OccupiedFallbackStarts).Add(PlayerStart);
		}
	}

	auto ChooseRandomStart = [](const TArray<APlayerStart*>& PlayerStarts)
	{
		return PlayerStarts.Num() > 0 ? PlayerStarts[FMath::RandHelper(PlayerStarts.Num())] : nullptr;
	};

	if (APlayerStart* Start = ChooseRandomStart(UnoccupiedTeamStarts)) return Start;
	if (APlayerStart* Start = ChooseRandomStart(OccupiedTeamStarts)) return Start;
	if (APlayerStart* Start = ChooseRandomStart(UnoccupiedFallbackStarts)) return Start;
	return ChooseRandomStart(OccupiedFallbackStarts);
}

AActor* ATeamGameMode::ChoosePlayerStart_Implementation(AController* Player)
{
	// Codex: ensure initial spawn and late join both have a team before choosing a start.
	ABlasterPlayerState* PlayerState = Player ? Player->GetPlayerState<ABlasterPlayerState>() : nullptr;
	AssignTeamIfNeeded(PlayerState);

	if (PlayerState)
	{
		bool bFoundAnyTeamPlayerStart = false;
		if (APlayerStart* TeamStart = ChooseTeamPlayerStart(Player, PlayerState->GetCurrentTeam(), bFoundAnyTeamPlayerStart))
		{
			return TeamStart;
		}
		if (bFoundAnyTeamPlayerStart)
		{
			// Codex: avoid falling through to Super, because Super can pick the opposing team's TeamPlayerStart.
			return nullptr;
		}
	}

	return Super::ChoosePlayerStart_Implementation(Player);
}

void ATeamGameMode::HandleMatchHasStarted()
{
	Super::HandleMatchHasStarted();

	ABlasterGameState* BlasterGameState = Cast<ABlasterGameState>(UGameplayStatics::GetGameState(this));
	if (BlasterGameState)
	{
		for (auto PState : BlasterGameState->PlayerArray)
		{
			ABlasterPlayerState* PlayerState = Cast<ABlasterPlayerState>(PState.Get());
			AssignTeamIfNeeded(PlayerState);
		}
	}
}

void ATeamGameMode::Logout(AController* Exiting)
{
	Super::Logout(Exiting);
	ABlasterGameState* BlasterGameState = Cast<ABlasterGameState>(UGameplayStatics::GetGameState(this));
	ABlasterPlayerState* PlayerState = Exiting ? Exiting->GetPlayerState<ABlasterPlayerState>() : nullptr;
	if (BlasterGameState && PlayerState)
	{
		if (BlasterGameState->RedTeamPlayers.Contains(PlayerState))
		{
			BlasterGameState->RedTeamPlayers.Remove(PlayerState);
		}
		if (BlasterGameState->BlueTeamPlayers.Contains(PlayerState))
		{
			BlasterGameState->BlueTeamPlayers.Remove(PlayerState);
		}
	}
}

void ATeamGameMode::PlayerEliminated(class AMyBlasterCharacter* ElimmedCharacter,
	class ABlasterPlayerController* VictimController, ABlasterPlayerController* AttackerController)
{
	Super::PlayerEliminated(ElimmedCharacter, VictimController, AttackerController);

	ABlasterGameState* BlasterGameState = Cast<ABlasterGameState>(UGameplayStatics::GetGameState(this));
	ABlasterPlayerState* Attacker = AttackerController ? Cast<ABlasterPlayerState>(AttackerController->PlayerState) : nullptr;
	if (BlasterGameState && Attacker)
	{
		if (Attacker->GetCurrentTeam() == ETeam::ET_RedTeam)
		{
			BlasterGameState->RedTeamScore();
		}
		if (Attacker->GetCurrentTeam() == ETeam::ET_BlueTeam)
		{
			BlasterGameState->BlueTeamScore();
		}
	}
}

void ATeamGameMode::PostLogin(APlayerController* PlayerController)
{
	Super::PostLogin(PlayerController);
	// Codex: assign the team before the controller's first pawn is spawned.
	ABlasterPlayerState* PlayerState = PlayerController ? PlayerController->GetPlayerState<ABlasterPlayerState>() : nullptr;
	AssignTeamIfNeeded(PlayerState);
}

void ATeamGameMode::RequestRespawn(ACharacter* ElimmedCharacter, AController* ElimmedController)
{
	if (ElimmedCharacter)
	{
		ElimmedCharacter->Reset();
		ElimmedCharacter->Destroy();
	}

	if (ElimmedController)
	{
		// Codex: force respawn through ChoosePlayerStart instead of reusing the previous StartSpot.
		ElimmedController->StartSpot = nullptr;
		AssignTeamIfNeeded(ElimmedController->GetPlayerState<ABlasterPlayerState>());
		RestartPlayer(ElimmedController);
	}
}

bool ATeamGameMode::ShouldSpawnAtStartSpot(AController* Player)
{
	// Codex: team mode must re-evaluate starts every spawn so team filters and occupancy are honored.
	return false;
}
