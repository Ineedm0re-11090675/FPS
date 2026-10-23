#if WITH_DEV_AUTOMATION_TESTS

#include "../BlasterComponent/CombatComponent.h"
#include "Blaster/Character/MyBlasterCharacter.h"
#include "Blaster/Weapon/Weapon.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCombatComponentTraceTargetTest,
	"Blaster.Combat.TraceTarget.ResolvesTraceMissToTraceEnd",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCombatComponentTraceTargetTest::RunTest(const FString& Parameters)
{
	const FVector TraceEnd(1000.f, 2000.f, 3000.f);

	FHitResult MissResult;
	const FVector MissTarget = UCombatComponent::ResolveTraceHitTarget(MissResult, TraceEnd);
	TestEqual(TEXT("Missed trace resolves to trace end"), MissTarget, TraceEnd);

	FHitResult HitResult;
	HitResult.bBlockingHit = true;
	HitResult.ImpactPoint = FVector(10.f, 20.f, 30.f);
	const FVector HitTarget = UCombatComponent::ResolveTraceHitTarget(HitResult, TraceEnd);
	TestEqual(TEXT("Blocking trace resolves to impact point"), HitTarget, HitResult.ImpactPoint);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCombatComponentCrosshairTargetTest,
	"Blaster.Combat.CrosshairTarget.ResolvesWeaponOwner",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCombatComponentCrosshairTargetTest::RunTest(const FString& Parameters)
{
	UWorld* TestWorld = UWorld::CreateWorld(EWorldType::Game, false);
	TestNotNull(TEXT("Test world is created"), TestWorld);
	if (TestWorld == nullptr) return false;

	AMyBlasterCharacter* TargetCharacter = TestWorld->SpawnActor<AMyBlasterCharacter>();
	AWeapon* OwnedWeapon = TestWorld->SpawnActor<AWeapon>();
	AWeapon* LooseWeapon = TestWorld->SpawnActor<AWeapon>();
	AActor* PlainActor = TestWorld->SpawnActor<AActor>();

	TestNotNull(TEXT("Target character is spawned"), TargetCharacter);
	TestNotNull(TEXT("Owned weapon is spawned"), OwnedWeapon);
	TestNotNull(TEXT("Loose weapon is spawned"), LooseWeapon);
	TestNotNull(TEXT("Plain actor is spawned"), PlainActor);

	if (TargetCharacter && OwnedWeapon)
	{
		OwnedWeapon->SetOwner(TargetCharacter);
	}

	TestTrue(
		TEXT("Character hit is a crosshair target"),
		UCombatComponent::IsCrosshairTargetActor(TargetCharacter)
	);
	TestTrue(
		TEXT("Weapon owned by character resolves to a crosshair target"),
		UCombatComponent::IsCrosshairTargetActor(OwnedWeapon)
	);
	TestFalse(
		TEXT("Loose weapon is not a crosshair target"),
		UCombatComponent::IsCrosshairTargetActor(LooseWeapon)
	);
	TestFalse(
		TEXT("Plain actor is not a crosshair target"),
		UCombatComponent::IsCrosshairTargetActor(PlainActor)
	);
	TestFalse(
		TEXT("Null actor is not a crosshair target"),
		UCombatComponent::IsCrosshairTargetActor(nullptr)
	);

	TestWorld->DestroyWorld(false);

	return true;
}

#endif
