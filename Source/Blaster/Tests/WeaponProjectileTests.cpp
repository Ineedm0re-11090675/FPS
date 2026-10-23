#if WITH_DEV_AUTOMATION_TESTS

#include "Blaster/Weapon/ProjectileRocket.h"
#include "Blaster/BlasterComponent/CombatComponent.h"
#include "Blaster/Weapon/Weapon.h"
#include "GameFramework/Character.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWeaponInheritanceTest,
	"Blaster.Weapon.Inheritance.WeaponIsActorNotCharacter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FWeaponInheritanceTest::RunTest(const FString& Parameters)
{
	TestTrue(
		TEXT("Weapon remains an actor"),
		AWeapon::StaticClass()->IsChildOf(AActor::StaticClass())
	);
	TestFalse(
		TEXT("Weapon should not inherit Character movement or capsule components"),
		AWeapon::StaticClass()->IsChildOf(ACharacter::StaticClass())
	);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRocketImpactReplicationTest,
	"Blaster.Projectile.RocketImpact.UsesReliableMulticastEffects",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FRocketImpactReplicationTest::RunTest(const FString& Parameters)
{
	UFunction* ImpactFunction = AProjectileRocket::StaticClass()->FindFunctionByName(TEXT("MulticastImpactEffects"));

	TestNotNull(TEXT("Rocket exposes a multicast impact effects RPC"), ImpactFunction);
	if (ImpactFunction == nullptr) return false;

	TestTrue(
		TEXT("Impact effects RPC is a net multicast"),
		ImpactFunction->FunctionFlags & FUNC_NetMulticast
	);
	TestTrue(
		TEXT("Impact effects RPC is reliable so every client gets explosion feedback"),
		ImpactFunction->FunctionFlags & FUNC_NetReliable
	);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCombatImpactReplicationTest,
	"Blaster.Combat.Impact.UsesReliableMulticastEffects",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FCombatImpactReplicationTest::RunTest(const FString& Parameters)
{
	UFunction* ImpactFunction = UCombatComponent::StaticClass()->FindFunctionByName(TEXT("MulticastImpactEffect"));

	TestNotNull(TEXT("Combat exposes a shared multicast impact effects RPC"), ImpactFunction);
	if (ImpactFunction == nullptr) return false;

	TestTrue(
		TEXT("Shared impact effects RPC is a net multicast"),
		ImpactFunction->FunctionFlags & FUNC_NetMulticast
	);
	TestTrue(
		TEXT("Shared impact effects RPC is reliable so clients see confirmed weapon impacts"),
		ImpactFunction->FunctionFlags & FUNC_NetReliable
	);

	return true;
}

#endif
